// all.cpp
// Single-file fixed server (C++17). Compile with:
// g++ -std=c++17 -O2 -pthread -o all all.cpp

#include <string>
#include <cstring>
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <optional>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <list>
#include <unordered_map>
#include <map>
#include <cstdlib>
#include <cerrno>

// --- Globals that must be visible early ---
inline std::atomic<bool> stop_all{false};
std::mutex job_mtx;
std::condition_variable job_cv;

// --- Config struct ---
struct SchedulerConfig {
    int scheduler_port = 0;
    int max_conn = 128;
    std::string scheduler_mode;
    std::string snni_dir;
    std::vector<std::string> models;
    std::vector<int> batch_sizes;
    std::string server_cmd_template;
    std::string client_cmd_template;
    std::string preproc_cmd_template;
    int preproc_regen_threshold = 1;
    int pre_base_port = 6000;
    int port_range = 100;
    int base_port = 7000;
    int default_threads = 4;
    int max_threads = 8;
    int preproc_timeout = 60;
    int inference_timeout = 300;
    std::string log_file = "job_log.csv";
    std::string sys_file;
    int total_cores = 8;
};

struct Job {
    char req_type = 'r'; // 'r' or 'a' or 'p'
    int clientSock = -1;
    std::string clientIP;
    int clientPort = 0;
    std::string model;
    int batch = 1;
    int threads = 1;

    long arrival_ts = 0;
    long start_ts = 0;
    long finish_ts = 0;

    std::string cmd;
    int exit_code = -1;

    enum class Status { Unknown, Completed, Failed } status = Status::Unknown;

    // optional for compatibility with older fragments
    int timeout = 0;
    double duration = 0.0;
};

struct CmdResult {
    int rc;
    std::string reason;
};

struct FileState {
    std::mutex mtx;           // protects this FileState
    bool is_generating = false;
    int reuse_count = 0;
};

struct ResourceManager {
    std::mutex mtx;        // protects counters
    int max_threads = std::thread::hardware_concurrency();
    int used_threads = 0;
    int running_jobs = 0;
    int queued_jobs  = 0;

    std::atomic<int> cpu_usage_percent{0};
    std::atomic<int> mem_usage_percent{0};
};

enum class ScheduleMode { FCFS, SJF };

// Forward declarations
class JobQueue;
class PortAllocator;
struct Worker;

static inline CmdResult spawnAndWaitShellDetailed(Job & job, int timeoutSec);
static inline CmdResult spawnAndWaitShellDetailed(const std::string &cmd, int timeoutSec);
void preProcThreads();

std::string getCmd(const std::string &cmd_template, const std::string snni_dir, int threads,
                    const std::string &model, int batch, int port, const std::string& ip="127.0.0.1");

inline std::string getPreProcCmd(const std::string &cmd_template, const std::string snni_dir,
                          int port, const std::string &model, int batch);

void handleClient(int clientSock, const std::string &ip, int port);
void dispatcherThread(JobQueue& jq);
void serverMainLoop(int listenPort);

inline long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// --- JobQueue class ---
class JobQueue {
public:
    JobQueue() { mode = ScheduleMode::FCFS; }
    void initJobQueue(const std::string& m) {
        if (m == "FCFS") mode = ScheduleMode::FCFS;
        else if (m == "SJF") mode = ScheduleMode::SJF;
        else throw std::invalid_argument("Unknown scheduling mode: " + m);
    }

    void push(const Job& job) {
        std::lock_guard<std::mutex> lock(mtx);

        if (mode == ScheduleMode::FCFS) {
            jobs.push_back(job);
        } else {
            auto it = std::find_if(jobs.begin(), jobs.end(),
                [&](const Job& other) {
                    if (job.batch == other.batch)
                        return job.arrival_ts < other.arrival_ts;
                    return job.batch < other.batch;
                });
            jobs.insert(it, job);
        }
        job_cv.notify_one();
    }

    // returns true if this job can be acquired (resources + file ready)
    bool canAcquire(const Job& job);

    // reserves resources & increments reuse_count for the job's file
    void acquire(const Job& job);

    std::optional<Job> getNextRunnableJob() {
        std::unique_lock<std::mutex> lock(mtx);

        while (true) {
            if (jobs.empty()) {
                job_cv.wait(lock, [&] { return !jobs.empty() || stop_all.load(); });
                if (stop_all.load()) return std::nullopt;
            }

            // scan for any job that can run (not just head of queue)
            for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                if (canAcquire(*it)) {
                    Job result = *it;
                    jobs.erase(it);
                    // commit reservations
                    acquire(result);
                    return result;
                }
            }

            // nothing runnable now: wait until signalled (preproc finished or resources freed)
            job_cv.wait(lock, [&] {
                if (stop_all.load()) return true;
                for (auto &j : jobs) {
                    if (canAcquire(j)) return true;
                }
                return false;
            });

            if (stop_all.load()) return std::nullopt;
        }
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx);
        return jobs.empty();
    }
    int size() {
        std::lock_guard<std::mutex> lock(mtx);
        return (int)jobs.size();
    }

private:
    mutable std::mutex mtx;
    std::list<Job> jobs;
    ScheduleMode mode;
};

// --- Worker: preproc worker thread holder ---
struct Worker {
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
    std::thread th;

    Worker() = default;
    ~Worker() {
        if (th.joinable()) th.join();
    }

    void signal() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            ready = true;
        }
        cv.notify_one();
    }

    void run(const std::string &key, const std::string &cmd, int preproc_timeout);
};

// --- Port allocator ---
class PortAllocator {
public:
    PortAllocator() : base_port(7000), port_range(100), next_offset(0) {}
    void initPortAllocator(int base, int range) {
        base_port = base;
        port_range = range;
        next_offset = 0;
    }

    int acquirePort() {
        std::lock_guard<std::mutex> lk(mtx);
        for (int i = 0; i < port_range; ++i) {
            int candidate = base_port + ((next_offset + i) % port_range);
            if (isPortFree(candidate)) {
                next_offset = (next_offset + i + 1) % port_range;
                return candidate;
            }
        }
        return -1;
    }

private:
    int base_port;
    int port_range;
    int next_offset;
    std::mutex mtx;

    bool isPortFree(int port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        bool free = (bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0);
        close(sock);
        return free;
    }
};

// --- Globals ---
inline SchedulerConfig sconfig;
inline ResourceManager g_res_mgr;
inline PortAllocator g_port_allocator;
JobQueue jobQueue;

// store workers and file states in pointer containers (no copy of mutex/cv)
inline std::unordered_map<std::string, std::unique_ptr<Worker>> preprocWorkers;
inline std::map<std::string, std::unique_ptr<FileState>> file_states;
inline std::mutex file_states_mtx;

// logging
std::mutex log_mtx;
std::ofstream log_file;

// --- Helpers (trim, split) ---
inline std::string trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(trim(token));
    }
    return tokens;
}

// --- Read config file (simple key=value) ---
bool readSchedulerConfigFile(const std::string &filename, SchedulerConfig &config) {
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "Failed to open config file: " << filename << std::endl;
        return false;
    }
    std::string line;
    while (std::getline(infile, line)) {
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));
        if (key == "SCHEDULER_PORT") config.scheduler_port = std::stoi(value);
        else if (key == "MAX_CONN") config.max_conn = std::stoi(value);
        else if (key == "SCHEDULER_MODE") config.scheduler_mode = value;
        else if (key == "SNNI_DIR") config.snni_dir = value;
        else if (key == "MODELS") config.models = split(value, ',');
        else if (key == "BATCH_SIZES") {
            auto parts = split(value, ',');
            for (auto &p : parts) config.batch_sizes.push_back(std::stoi(p));
        }
        else if (key == "SERVER_CMD_TEMPLATE") config.server_cmd_template = value;
        else if (key == "CLIENT_CMD_TEMPLATE") config.client_cmd_template = value;
        else if (key == "PREPROC_CMD_TEMPLATE") config.preproc_cmd_template = value;
        else if (key == "PREPROC_REGEN_THRESHOLD") config.preproc_regen_threshold = std::stoi(value);
        else if (key == "PRE_BASE_PORT") config.pre_base_port = std::stoi(value);
        else if (key == "PORT_RANGE") config.port_range = std::stoi(value);
        else if (key == "BASE_PORT") config.base_port = std::stoi(value);
        else if (key == "DEFAULT_THREADS") config.default_threads = std::stoi(value);
        else if (key == "MAX_THREADS") config.max_threads = std::stoi(value);
        else if (key == "PREPROC_TIMEOUT") config.preproc_timeout = std::stoi(value);
        else if (key == "INFERENCE_TIMEOUT") config.inference_timeout = std::stoi(value);
        else if (key == "LOG_FILE") config.log_file = value;
        else if (key == "SYS_FILE") config.sys_file = value;
        else if (key == "TOTAL_CORES") config.total_cores = std::stoi(value);
        else {
            std::cerr << "Unknown config key: " << key << std::endl;
        }
    }
    infile.close();
    return true;
}

void initLogger(const std::string& path) {
    std::lock_guard<std::mutex> lk(log_mtx);
    if (log_file.is_open()) log_file.close();
    log_file.open(path, std::ios::app);
    if (!log_file.is_open()) {
        std::cerr << "Failed to open log file: " << path << std::endl;
        return;
    }
    if (log_file.tellp() == 0) {
        log_file << "arrival_ts;start_ts;finish_ts;client;model_batch;threads;cmd;exit_code;status;type\n";
        log_file.flush();
    }
}

void logJob(const Job& job) {
    std::lock_guard<std::mutex> lk(log_mtx);
    if (!log_file.is_open()) return;
    log_file << job.arrival_ts << ";"
             << job.start_ts << ";"
             << job.finish_ts << ";"
             << job.clientIP << ":" << job.clientPort << ";"
             << job.model << "_" << job.batch << ";"
             << job.threads << ";"
             << "\"" << job.cmd << "\";"
             << job.exit_code << ";"
             << (job.status == Job::Status::Completed ? "completed" : "failed") << ";"
             << job.req_type << "\n";
    log_file.flush();
}

// --- simple system monitor (updates resource manager counts) ---
double getCpuUsage() {
    static long prevIdle = 0, prevTotal = 0;
    std::ifstream stat("/proc/stat");
    std::string line;
    if (!std::getline(stat, line)) return 0;
    std::istringstream ss(line);
    std::string cpu;
    long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
    ss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    long idleTime = idle + iowait;
    long totalTime = user + nice + system + idle + iowait + irq + softirq + steal;
    long diffIdle = idleTime - prevIdle;
    long diffTotal = totalTime - prevTotal;
    prevIdle = idleTime;
    prevTotal = totalTime;
    if (diffTotal == 0) return 0.0;
    return (100.0 * (diffTotal - diffIdle)) / diffTotal;
}
double getMemoryUsageMB() {
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    long value;
    std::string unit;
    long memTotal = 0, memAvailable = 0;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemTotal:") memTotal = value;
        if (key == "MemAvailable:") memAvailable = value;
    }
    if (memTotal == 0) return 0.0;
    return (double)(memTotal - memAvailable) / 1024.0; // MB
}
void monitorSystem() {
    while (!stop_all.load()) {
        g_res_mgr.cpu_usage_percent = (int)getCpuUsage();
        g_res_mgr.mem_usage_percent = (int)getMemoryUsageMB();
        {
            std::lock_guard<std::mutex> lk(g_res_mgr.mtx);
            g_res_mgr.queued_jobs = jobQueue.size();
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
int serverSock = -1;
// --- signal handler ---
void signalHandler(int) {
    stop_all.store(true);
    close(serverSock); // make accept fail
    job_cv.notify_all();
    // notify preproc workers
    std::lock_guard<std::mutex> lk(file_states_mtx);
    for (auto &p : preprocWorkers) {
        if (p.second) p.second->cv.notify_all();
    }
}

// --- spawn helpers (real implementations) ---
inline std::vector<char*> makeArgv(const std::string &cmd, std::vector<std::string> &storage) {
    std::istringstream iss(cmd);
    std::string token;
    while (iss >> token) storage.push_back(token);
    std::vector<char*> argv;
    for (auto &s : storage) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    return argv;
}

static inline CmdResult spawnAndWaitShellDetailed(Job & job, int timeoutSec) {
    CmdResult result{0, "ok"};
    pid_t pid = fork();
    if (pid < 0) {
        result.rc = -3;
        result.reason = "fork_failed";
        return result;
    }
    if (pid == 0) {
        // child
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        if (!sconfig.snni_dir.empty()) {
            if (chdir(sconfig.snni_dir.c_str()) != 0) _exit(126);
        }
        if (job.threads > 0) {
            setenv("OMP_NUM_THREADS", std::to_string(job.threads).c_str(), 1);
        }
        std::vector<std::string> storage;
        auto argv = makeArgv(job.cmd, storage);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    // parent
    auto start = std::chrono::steady_clock::now();
    int status;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) {
                result.rc = WEXITSTATUS(status);
                result.reason = "exit_" + std::to_string(result.rc);
            } else if (WIFSIGNALED(status)) {
                result.rc = -1;
                result.reason = "signal_" + std::to_string(WTERMSIG(status));
            }
            break;
        }
        if (timeoutSec > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() >= timeoutSec) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.rc = -2;
            result.reason = "timeout";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return result;
}

static inline CmdResult spawnAndWaitShellDetailed(const std::string &cmd, int timeoutSec) {
    CmdResult result{0, "ok"};
    pid_t pid = fork();
    if (pid < 0) {
        result.rc = -3; result.reason = "fork_failed"; return result;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    }
    auto start = std::chrono::steady_clock::now();
    int status;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) {
                result.rc = WEXITSTATUS(status);
                result.reason = "exit_" + std::to_string(result.rc);
            } else if (WIFSIGNALED(status)) {
                result.rc = -1;
                result.reason = "signal_" + std::to_string(WTERMSIG(status));
            }
            break;
        }
        if (timeoutSec > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() >= timeoutSec) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.rc = -2;
            result.reason = "timeout";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return result;
}

// --- getCmd / getPreProcCmd (string template replacements) ---
std::string getCmd(const std::string &cmd_template, const std::string snni_dir, int threads,
                   const std::string &model, int batch, int port, const std::string& ip) {
    std::string cmd = cmd_template;
    size_t pos;
    while ((pos = cmd.find("{SNNI_DIR}")) != std::string::npos) cmd.replace(pos, 10, snni_dir);
    while ((pos = cmd.find("{THREADS}")) != std::string::npos) cmd.replace(pos, 9, std::to_string(threads));
    while ((pos = cmd.find("{MODEL}")) != std::string::npos) cmd.replace(pos, 7, model);
    while ((pos = cmd.find("{BATCH}")) != std::string::npos) cmd.replace(pos, 7, std::to_string(batch));
    while ((pos = cmd.find("{PORT}")) != std::string::npos) cmd.replace(pos, 6, std::to_string(port));
    while ((pos = cmd.find("{SERVER_IP}")) != std::string::npos) cmd.replace(pos, 11, ip);
    return cmd;
}

inline std::string getPreProcCmd(const std::string &cmd_template, const std::string snni_dir,
                          int port, const std::string &model, int batch) {
    std::string cmd = cmd_template;
    size_t pos;
    while ((pos = cmd.find("{SNNI_DIR}")) != std::string::npos) cmd.replace(pos, 10, snni_dir);
    while ((pos = cmd.find("{MODEL}")) != std::string::npos) cmd.replace(pos, 7, model);
    while ((pos = cmd.find("{PORT}")) != std::string::npos) cmd.replace(pos, 6, std::to_string(port));
    while ((pos = cmd.find("{BATCH}")) != std::string::npos) cmd.replace(pos, 7, std::to_string(batch));
    return cmd;
}

// --- preProc Threads: create workers & FileState entries and start workers ---
void preProcThreads() {
    int nextport = 0;
    for (const auto &model : sconfig.models) {
        for (int batch : sconfig.batch_sizes) {
            std::string key = model + "_" + std::to_string(batch);
            int port = sconfig.pre_base_port + ++nextport;
            std::string cmd = getPreProcCmd(sconfig.preproc_cmd_template, sconfig.snni_dir, port, model, batch);

            // create file state if missing and mark generating
            {
                std::lock_guard<std::mutex> lk(file_states_mtx);
                if (file_states.find(key) == file_states.end()) {
                    file_states[key] = std::make_unique<FileState>();
                }
                // mark generating prior to waking worker
                {
                    std::lock_guard<std::mutex> fslk(file_states[key]->mtx);
                    file_states[key]->is_generating = true;
                    file_states[key]->reuse_count = 0;
                }
                // create worker if missing
                if (preprocWorkers.find(key) == preprocWorkers.end()) {
                    preprocWorkers[key] = std::make_unique<Worker>();
                }
            }

            // start worker thread
            preprocWorkers[key]->run(key, cmd, sconfig.preproc_timeout);
            preprocWorkers[key]->signal();
        }
    }
}

// --- Worker::run implementation ---
void Worker::run(const std::string &key, const std::string &cmd, int preproc_timeout) {
    // Launch worker thread
    th = std::thread([this, key, cmd, preproc_timeout]() {
        // get pointer to FileState (must exist)
        FileState* fs = nullptr;
        {
            std::lock_guard<std::mutex> lk(file_states_mtx);
            auto it = file_states.find(key);
            if (it == file_states.end()) {
                // nothing to do; exit thread
                return;
            }
            fs = it->second.get();
        }

        while (!stop_all.load()) {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [this]() { return ready || stop_all.load(); });

            if (stop_all.load()) break;
            ready = false;
            lk.unlock();

            // mark generating
            {
                std::lock_guard<std::mutex> fslk(fs->mtx);
                fs->is_generating = true;
                fs->reuse_count = 0;
            }

            // build a preproc job record for logging
            Job preJob;
            preJob.req_type = 'p';
            preJob.cmd = cmd;
            preJob.model = key;
            preJob.start_ts = nowMs();

            // run preproc command (shell)
            CmdResult cres = spawnAndWaitShellDetailed(cmd, preproc_timeout);

            preJob.finish_ts = nowMs();
            preJob.exit_code = cres.rc;
            preJob.status = (cres.rc == 0 ? Job::Status::Completed : Job::Status::Failed);

            if (cres.rc != 0) {
                std::cerr << "Preproc failed (" << cmd << "): " << cres.reason << "\n";
            } else {
                std::cerr << "Preproc done (" << key << ")\n";
            }

            // mark not generating
            {
                std::lock_guard<std::mutex> fslk(fs->mtx);
                fs->is_generating = false;
            }

            // notify dispatcher that file is ready
            job_cv.notify_all();
            logJob(preJob);
        }
    });
}

// --- JobQueue canAcquire / acquire implementations ---
bool JobQueue::canAcquire(const Job& job) {
    std::string key = job.model + "_" + std::to_string(job.batch);

    FileState* pfs = nullptr;
    {
        std::lock_guard<std::mutex> map_lk(file_states_mtx);
        auto it = file_states.find(key);
        if (it == file_states.end()) return false;
        pfs = it->second.get();
    }
    FileState &fs = *pfs;

    // lock both resource manager and file state to inspect atomically
    std::unique_lock<std::mutex> lk_res(g_res_mgr.mtx, std::defer_lock);
    std::unique_lock<std::mutex> lk_file(fs.mtx, std::defer_lock);
    std::lock(lk_res, lk_file);

    if (g_res_mgr.used_threads + job.threads > g_res_mgr.max_threads) return false;
    if (fs.is_generating || fs.reuse_count >= sconfig.preproc_regen_threshold) return false;

    return true;
}

void JobQueue::acquire(const Job& job) {
    std::string key = job.model + "_" + std::to_string(job.batch);

    FileState* pfs = nullptr;
    {
        std::lock_guard<std::mutex> map_lk(file_states_mtx);
        auto it = file_states.find(key);
        if (it == file_states.end()) {
            // should not happen if canAcquire was true
            throw std::runtime_error("acquire: file_state missing for " + key);
        }
        pfs = it->second.get();
    }
    FileState &fs = *pfs;

    std::unique_lock<std::mutex> lk_res(g_res_mgr.mtx, std::defer_lock);
    std::unique_lock<std::mutex> lk_file(fs.mtx, std::defer_lock);
    std::lock(lk_res, lk_file);

    g_res_mgr.used_threads += job.threads;
    g_res_mgr.running_jobs++;
    g_res_mgr.queued_jobs = std::max(0, g_res_mgr.queued_jobs - 1);

    fs.reuse_count++;
}

// --- Dispatcher (main dispatcher thread) ---
void dispatcherThread(JobQueue& jq) {
    while (!stop_all.load()) {
        std::optional<Job> jobOpt = jq.getNextRunnableJob();
        if (!jobOpt) break; // stop_all possibly triggered

        Job job = *jobOpt;

        int available_threads = g_res_mgr.max_threads - g_res_mgr.used_threads;

        int assignedPort = g_port_allocator.acquirePort();
        if (available_threads <= 0 || assignedPort == -1) {
            // release (should not happen often)
            // use a release-like procedure: decrement counts and re-enqueue
            {
                // reverse acquire effect
                std::string key = job.model + "_" + std::to_string(job.batch);
                FileState* pfs = nullptr;
                {
                    std::lock_guard<std::mutex> map_lk(file_states_mtx);
                    auto it = file_states.find(key);
                    if (it != file_states.end()) pfs = it->second.get();
                }
                if (pfs) {
                    std::unique_lock<std::mutex> lk_res(g_res_mgr.mtx, std::defer_lock);
                    std::unique_lock<std::mutex> lk_file(pfs->mtx, std::defer_lock);
                    std::lock(lk_res, lk_file);
                    g_res_mgr.used_threads = std::max(0, g_res_mgr.used_threads - job.threads);
                    g_res_mgr.running_jobs = std::max(0, g_res_mgr.running_jobs - 1);
                    // don't decrement reuse_count
                } else {
                    std::lock_guard<std::mutex> lk(g_res_mgr.mtx);
                    g_res_mgr.used_threads = std::max(0, g_res_mgr.used_threads - job.threads);
                    g_res_mgr.running_jobs = std::max(0, g_res_mgr.running_jobs - 1);
                }
            }

            // requeue
            jq.push(job);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // pick threads for job
        if (job.batch < 6) job.threads = std::min(sconfig.default_threads, available_threads);
        else job.threads = std::min(sconfig.max_threads, available_threads);

        // respond to client with port and threads
        if (job.clientSock >= 0) {
            std::string resp = "PORT:" + std::to_string(assignedPort) + ";THREADS:" + std::to_string(job.threads);
            send(job.clientSock, resp.c_str(), (int)resp.size(), 0);
            close(job.clientSock);
        }

        // spawn the inference thread
        std::thread([job, assignedPort]() mutable {
            auto cmd = getCmd(sconfig.server_cmd_template, sconfig.snni_dir, job.threads, job.model, job.batch, assignedPort);
            job.cmd = cmd;

            // start timestamps and run
            {
                std::lock_guard<std::mutex> lk(g_res_mgr.mtx);
                // used_threads already reserved in acquire() earlier
                job.start_ts = nowMs();
            }

            auto res = spawnAndWaitShellDetailed(job, sconfig.inference_timeout);

            {
                std::lock_guard<std::mutex> lk(g_res_mgr.mtx);
                g_res_mgr.used_threads = std::max(0, g_res_mgr.used_threads - job.threads);
                g_res_mgr.running_jobs = std::max(0, g_res_mgr.running_jobs - 1);
            }

            job.finish_ts = nowMs();
            job.exit_code = res.rc;
            job.status = (res.rc == 0 ? Job::Status::Completed : Job::Status::Failed);

            logJob(job);

            // After job finishes, check if file needs regen and notify worker if so
            std::string key = job.model + "_" + std::to_string(job.batch);
            {
                std::lock_guard<std::mutex> map_lk(file_states_mtx);
                auto it = file_states.find(key);
                if (it != file_states.end()) {
                    FileState &fs = *it->second;
                    std::lock_guard<std::mutex> fslk(fs.mtx);
                    if (fs.reuse_count >= sconfig.preproc_regen_threshold) {
                        fs.is_generating = true;
                        fs.reuse_count = 0;
                        auto wit = preprocWorkers.find(key);
                        if (wit != preprocWorkers.end()) wit->second->signal();
                    }
                }
            }

            // notify dispatcher waiting threads
            job_cv.notify_all();
        }).detach();
    }
}

// --- Client handler (parses request and enqueues job) ---
void handleClient(int clientSock, const std::string &ip, int port) {
    char buffer[512] = {0};
    int r = read(clientSock, buffer, sizeof(buffer) - 1);
    if (r <= 0) { close(clientSock); return; }
    buffer[r] = '\0';
    std::string req(buffer);
    req.erase(std::remove_if(req.begin(), req.end(),
                [](unsigned char c){ return c=='\n' || c=='\r'; }),
              req.end());
    if (req.size() < 3) { close(clientSock); return; }
    char req_type = req[0];
    if (req_type != 'r' && req_type != 'a') {
        std::cerr<< "wrong request type " <<req_type<<std::endl;
        close(clientSock);
        return;
    }
    std::string model_batch = req.substr(2); // skip "<type>_"
    auto us = model_batch.find("_");
    if (us == std::string::npos) { close(clientSock); return; }
    std::string model = model_batch.substr(0, us);
    int batch = 1;
    try { batch = std::stoi(model_batch.substr(us + 1)); } catch (...) { batch = 1; }

    Job job;
    job.req_type = req_type;
    job.clientSock = clientSock;
    job.clientIP = ip;
    job.clientPort = port;
    job.model = model;
    job.batch = batch;
    job.threads = sconfig.default_threads;
    job.arrival_ts = nowMs();

    jobQueue.push(job);
    {
        std::lock_guard<std::mutex> lk(g_res_mgr.mtx);
        g_res_mgr.queued_jobs = jobQueue.size();
    }

    std::cerr << "[Server] Job queued (" << req_type << "): " << model << "_" << batch
              << " from " << ip << ":" << port << "\n";
}

// --- Server main loop (listen + accept) ---

void serverMainLoop(int listenPort) {
    serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listenPort);

    if (bind(serverSock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(serverSock); return; }
    if (listen(serverSock, sconfig.max_conn) < 0) { perror("listen"); close(serverSock); return; }

    std::cerr << "[Server] Listening on port " << listenPort << " ...\n";

    while (!stop_all.load()) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int csock = accept(serverSock, (sockaddr*)&caddr, &clen);
        if (csock < 0) {
            if (!stop_all.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
            else break;
        }
        std::string cip = inet_ntoa(caddr.sin_addr);
        int cport = ntohs(caddr.sin_port);
        std::thread(handleClient, csock, cip, cport).detach();
    }
    close(serverSock);
    std::cerr << "[Server] Shut down.\n";
}

// --- main ---
int main() {
    if (!readSchedulerConfigFile("config.cfg", sconfig)) {
        std::cout << "Error reading config file (config.cfg). Using defaults." << std::endl;
        // continue with defaults
    }

    jobQueue.initJobQueue(sconfig.scheduler_mode);
    g_port_allocator.initPortAllocator(sconfig.base_port, sconfig.port_range);
    g_res_mgr.max_threads = sconfig.total_cores;

    initLogger(sconfig.log_file);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::thread(monitorSystem).detach();

    // ensure file_states and preprocWorkers map entries are created before starting workers
    {
        std::lock_guard<std::mutex> lk(file_states_mtx);
        for (const auto &m : sconfig.models) {
            for (int b : sconfig.batch_sizes) {
                std::string key = m + "_" + std::to_string(b);
                if (file_states.find(key) == file_states.end()) {
                    file_states[key] = std::make_unique<FileState>();
                }
                if (preprocWorkers.find(key) == preprocWorkers.end()) {
                    preprocWorkers[key] = std::make_unique<Worker>();
                }
            }
        }
    }

    preProcThreads();

    // start dispatcher thread
    std::thread(dispatcherThread, std::ref(jobQueue)).detach();

    // run server accept loop (blocking)
    serverMainLoop(sconfig.scheduler_port);

    // cleanup - wait a bit for threads to notice stop_all
    stop_all.store(true);
    job_cv.notify_all();
    {
        std::lock_guard<std::mutex> lk(file_states_mtx);
        for (auto &p : preprocWorkers) { if (p.second) p.second->cv.notify_all(); }
    }

    return 0;
}
