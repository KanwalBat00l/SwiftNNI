// Client.cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <signal.h>   // *** added: for signal()
#include <cstdlib>    // *** added: for setenv(), atoi(), std::to_string
#include <errno.h>    // *** added: errno if you want to check errors

struct Request{
  std::string server_ip ;
  int server_port ;
  std::string req_type ;  // "r" or "a"
  std::string model_batch ;
} request; // note: this defines a global `request`

bool Parse (char** argv, Request & request){
    request.server_ip = argv[1];
    request.server_port = atoi(argv[2]);
    request.req_type = argv[3];  // "r" or "a"
    request.model_batch = argv[4];

    if (request.req_type != "r" && request.req_type != "a") {
        std::cerr << "Request type must be 'r' (request) or 'a' (advance)\n";
        return false;}
    return true;
}

// Config Client
struct ClientConfig {
    std::string snni_dir;
    std::string client_cmd_template;
    int inference_timeout = 0;
};

// Helper function to trim whitespace
inline std::string trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

// Split string by delimiter
std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(trim(token));
    }
    return tokens;
}

// Function to read config file
bool readClientConfigFile(const std::string &filename, ClientConfig &config) {
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "Failed to open config file: " << filename << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(infile, line)) {
        // Remove comments
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        line = trim(line);
        if (line.empty()) continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));

        if (key == "SNNI_DIR") config.snni_dir = value;
        else if (key == "CLIENT_CMD_TEMPLATE") config.client_cmd_template = value;
        else if (key == "INFERENCE_TIMEOUT") config.inference_timeout = std::stoi(value);
        // else {
        //     std::cerr << "Unknown config key: " << key << std::endl;
        // }
    }

    infile.close();
    return true;
}

std::string getCmd(const std::string &cmd_template, const std::string snni_dir, int threads,
                    const std::string &model, int batch,int port, const std::string& ip="127.0.0.1")
{
    std::string cmd = cmd_template; // make a copy

    size_t pos;
    // Replace {SNNI_DIR}
    while ((pos = cmd.find("{SNNI_DIR}")) != std::string::npos) {
        cmd.replace(pos, 10, snni_dir);
    }

    while ((pos = cmd.find("{THREADS}")) != std::string::npos) {
        cmd.replace(pos, 9, std::to_string(threads));
    }

    // Replace {MODEL}
    while ((pos = cmd.find("{MODEL}")) != std::string::npos) {
        cmd.replace(pos, 7, model);
    }

    // Replace {BATCH}
    while ((pos = cmd.find("{BATCH}")) != std::string::npos) {
        cmd.replace(pos, 7, std::to_string(batch));
    }

    // Replace {PORT}
    while ((pos = cmd.find("{PORT}")) != std::string::npos) {
        cmd.replace(pos, 6, std::to_string(port));
    }
    // Replace {IP} if any
    while ((pos = cmd.find("{SERVER_IP}")) != std::string::npos) {
        cmd.replace(pos, 11, ip);
    }

    return cmd;
}

struct CmdResult {
    int rc;
    std::string reason;
};

// Helper: split command string into argv[]
inline std::vector<char*> makeArgv(const std::string &cmd, std::vector<std::string> &storage) {
    std::istringstream iss(cmd);
    std::string arg;
    while (iss >> arg) {
        storage.push_back(arg);
    }
    std::vector<char*> argv;
    for (auto &s : storage) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    return argv;
}

// Safer alternative to spawnAndWaitShellDetailed
inline CmdResult spawnAndWaitDetailed(const std::string &workDir,
                                      const std::string &cmd,
                                      int timeoutSec,
                                      const std::string &ompThreads = "4") {
    CmdResult result{0, "ok"};
    pid_t pid = fork();
    if (pid < 0) {
        result.rc = -3;
        result.reason = "fork_failed";
        return result;
    }

    if (pid == 0) {
        // --- Child process ---

        // Close extra FDs (optional safety)
        for (int fd = 3; fd < 1024; ++fd) close(fd);

        // Reset signals
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        // Change working directory to shark
        if (chdir(workDir.c_str()) != 0) {
            _exit(126); // cannot chdir
        }

        // Set environment variable OMP_NUM_THREADS
        setenv("OMP_NUM_THREADS", ompThreads.c_str(), 1);

        // Build argv
        std::vector<std::string> storage;
        auto argv = makeArgv(cmd, storage);

        execvp(argv[0], argv.data());
        _exit(127); // exec failed
    }

    // --- Parent process ---
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
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count() >= timeoutSec) {
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

//
inline CmdResult spawnAndWaitShellDetailed(const std::string &cmd, int timeoutSec) {
    CmdResult result{0, "ok"};
    pid_t pid = fork();
    if (pid < 0) {
        result.rc = -3;
        result.reason = "fork_failed";
        return result;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127); // exec failed
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
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count() >= timeoutSec) {
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



int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: SchedulerClient <server_ip> <server_port> <r|a> <model_batch>\n";
        return 1;
    }
    //1. Parse CommandLine
    if (!Parse(argv, request)) {
        return 1;
    }

    //2. Read the config file
    ClientConfig cfg;
    if (!readClientConfigFile("config.cfg", cfg)) {
        std::cout << "Error reading client config file"<< std::endl;
        return 0;
    }



    //3.  Connect 
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(request.server_port);
    inet_pton(AF_INET, request.server_ip.c_str(), &serv.sin_addr);
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) { perror("connect"); close(sock); return 1; }

    //4. ---- send request ----
    std::string msg = request.req_type + std::string("_") + request.model_batch;
    std::cout<<msg;
    send(sock, msg.c_str(), msg.size(), 0);

    // set timeout of 5s for recv
    struct timeval tv;
    tv.tv_sec = cfg.inference_timeout*4;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    //5. ---- receive response ----
    char buf[512]; 
    ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
    if (n <= 0) { std::cerr << "No response from scheduler\n"; close(sock); return 1; }
    buf[n] = 0;
    std::string resp(buf);
    std::cout<<resp<<std::endl;
    if (resp.rfind("PORT:",0)!=0) { std::cerr<<"Invalid response\n"; close(sock); return 1; }
    int port=0, threads=0;
    auto pos = resp.find(';');
    if (pos!=std::string::npos) {
        port = stoi(resp.substr(5,pos-5));
        auto p2 = resp.find("THREADS:", pos+1);
        if (p2!=std::string::npos) threads = stoi(resp.substr(p2+8));
    } else {
        port = stoi(resp.substr(5));
    }
    close(sock);

    // ---- launch job ----
    auto up = request.model_batch.find('_');
    std::string model = request.model_batch.substr(0,up);
    int batch = atoi(request.model_batch.substr(up+1).c_str());

    std::string cmd = getCmd(cfg.client_cmd_template, cfg.snni_dir, threads,
                     model, batch,port, request.server_ip); 
    // std::cout<<cmd<<std::endl;
    // auto res = spawnAndWaitShellDetailed(cmd, cfg.inference_timeout);

    // *** fixed: pass ompThreads as string (was 'atoi(threads)' which is invalid)
    auto res = spawnAndWaitDetailed(
        cfg.snni_dir,        // working dir
        cmd,                 // command
        cfg.inference_timeout, // timeout seconds
        std::to_string(threads) // OMP_NUM_THREADS as string
    );

    std::cout << "rc=" << res.rc << " reason=" << res.reason << "\n";

    // ---- minimal logging (always with port) ----
    if (res.rc!=0) {
        std::cerr << "[Client port " << port << "] Inference failed rc=" 
                  << res.rc << " reason=" << res.reason << "\n";
    } else {
        std::cerr << "[Client port " << port << "] Inference finished ok\n";
    }

    std::ofstream log("scheduler_client_log.txt", std::ios::app);
    log << request.req_type << "_" << request.model_batch << ";" << ";port=" << port << ";rc=" << res.rc << "\n";

    return res.rc;
}
