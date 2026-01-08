#include "SAPPISServer.hpp"
#include "StringUtils.hpp"
#include "ProcessLauncher.hpp"
#include "SystemMonitor.hpp"
#include "ConfigManager.hpp"
#include "SchedulerFactory.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

/**
 * Helper: Current timestamp in ms.
 */
static long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

SAPPISServer::SAPPISServer(SystemConfig config, std::map<std::string, ModelProfile> model_profiles)
    : sys(config), profiles(model_profiles) {
    
    // 1. Initialize core managers
    rm = std::make_unique<ResourceManager>(sys.total_cores, sys.base_port, sys.port_range, 
                                           sys.max_preproc_concurrency, sys.enable_core_pinning);
    fm = std::make_unique<FileManager>();
    logger = std::make_unique<Logger>(sys.log_file);

    // 2. Instantiate scheduler via Factory (Decoupled design)
    scheduler = SchedulerFactory::create(sys.scheduler_mode, sys.aging_factor);
}

void SAPPISServer::start() {
    running = true;
    std::cout << "[SAPPIS] Engine running mode: " << sys.scheduler_mode << std::endl;

    // Launch Background Loops
    std::thread d_thread(&SAPPISServer::dispatcherLoop, this);
    std::thread r_thread(&SAPPISServer::replenisherLoop, this);

    // Main thread handles Admission Control
    listenerLoop();

    if (d_thread.joinable()) d_thread.join();
    if (r_thread.joinable()) r_thread.join();
}

void SAPPISServer::stop() {
    running = false;
    saveDynamicProfile(); // Persistence on shutdown
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
    }
}

/**
 * REPLENISHER: Proactive Pre-processing.
 * Ensures model buffers are filled by spawning Dealer processes.
 */
void SAPPISServer::replenisherLoop() {
    while (running) {
        for (auto& [key, p] : profiles) {
            if (!running) break;
            if (fm->getActiveCount(key) < p.max_buffer) {
                if (rm->hasCapacityForDealer()) {
                    std::string prefix = fm->initiateFile(key);
                    int dealer_core = rm->acquireDealerCore(); 
                    std::string d_core_str = (dealer_core != -1) ? std::to_string(dealer_core) : "";

                    std::string cmd = StringUtils::buildCommand(sys.preproc_cmd_template, sys.snni_dir, 
                                                               p.model, p.batch, 0, prefix, "127.0.0.1", d_core_str);

                    std::thread([this, cmd, key, prefix, p, dealer_core]() {
                        int timeout = (p.preproc_ms / 1000) * 2 + 30;
                        ProcessLauncher::runShellCommand(cmd, timeout);
                        fm->setReady(key, prefix);
                        rm->releaseDealerSlot();
                        if (dealer_core != -1) rm->releaseCores({dealer_core});
                    }).detach();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

/**
 * LISTENER: Admission Control Handshake.
 */
void SAPPISServer::listenerLoop() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(sys.scheduler_port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("Fatal: Bind failed on port " + std::to_string(sys.scheduler_port));
    }
    listen(server_fd, sys.max_conn);

    while (running) {
        sockaddr_in c_addr;
        socklen_t addr_len = sizeof(c_addr);
        int c_sock = accept(server_fd, (struct sockaddr*)&c_addr, &addr_len);
        if (c_sock < 0) continue;

        char buf[1024] = {0};
        int valread = read(c_sock, buf, 1023);
        if (valread <= 0) { close(c_sock); continue; }

        std::string request_str = ConfigManager::trim(buf);
        std::stringstream ss(request_str);
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(ss, part, '_')) if (!part.empty()) parts.push_back(part);

        if (parts.size() < 4) {
            send(c_sock, "REJECTED_FORMAT", 15, 0);
            close(c_sock);
            continue;
        }

        std::string key = parts[1] + "_" + parts[2];
        long requested_slo = std::stol(parts[3]);

        if (profiles.find(key) == profiles.end()) {
            send(c_sock, "REJECTED_MODEL", 14, 0);
            close(c_sock);
            continue;
        }

        // Admission Logic based on Dynamic EMA
        long expected_ms = profiles.at(key).dynamic_inf_ms.load();
        if (requested_slo >= static_cast<long>(expected_ms * sys.default_slo_k_factor)) {
            Job new_job;
            new_job.type = 'r';
            new_job.client_sock = c_sock;
            new_job.model = parts[1];
            new_job.batch = std::stoi(parts[2]);
            new_job.arrival_ts = nowMs();
            new_job.requested_slo_ms = requested_slo;

            send(c_sock, "ACCEPTED", 8, 0);
            scheduler->push(new_job);
        } else {
            send(c_sock, "REJECTED_SLO_UNFEASIBLE", 23, 0);
            close(c_sock);
        }
    }
}

/**
 * DISPATCHER: Concurrent Execution and Resource Management.
 */
void SAPPISServer::dispatcherLoop() {
    // Detect system capacity once
    SystemSnapshot initial_snap = SystemMonitor::takeSnapshot();
    const double node_ram_limit = initial_snap.total_mem_gb;

    while (running) {
        // --- HO BLOCKING MITIGATION ---
        // popReadyJob now handles the File + Priority search.
        auto job_opt = scheduler->popReadyJob(*fm, profiles, node_ram_limit);

        if (!job_opt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        Job j = *job_opt;
        std::string key = j.model + "_" + std::to_string(j.batch);

        // --- ELASTIC RESOURCE ALLOCATION ---
        // We take as many cores as possible. Starvation is prevented.
        j.assigned_cores = rm->acquireCoresElastic(profiles.at(key).threads);
        j.assigned_port = rm->acquirePort();
        j.start_ts = nowMs();

        // Dispatch Worker Thread
        std::thread([this, j, key]() mutable {
            // Fix: Use the reference to calculate timeout and learned average
            ModelProfile& prof = profiles.at(key); 
            
            std::string core_str = rm->coresToString(j.assigned_cores);
            std::string cmd = StringUtils::buildCommand(sys.server_cmd_template, sys.snni_dir, 
                                                       j.model, j.batch, j.assigned_port, 
                                                       j.assigned_file, sys.server_ip, core_str);

            // Step 1: Handshake
            std::stringstream ss;
            ss << "START_INF:" << sys.server_ip << ":" << j.assigned_port << ":" 
               << j.model << ":" << j.batch << ":" << j.assigned_file;
            send(j.client_sock, ss.str().c_str(), ss.str().length(), 0);
            close(j.client_sock);

            // Step 2: Safety Execution
            // Fix: Access dynamic_inf_ms via 'prof' to clear warning
            long expected_inf = prof.dynamic_inf_ms.load();
            int safety_timeout = static_cast<int>((expected_inf / 1000) * 5 + 60);
            if (safety_timeout < 120) safety_timeout = 120;

            auto start_exec = std::chrono::steady_clock::now();
            int rc = ProcessLauncher::runShellCommand(cmd, safety_timeout);
            auto end_exec = std::chrono::steady_clock::now();

            // Step 3: Measurement
            long obs = std::chrono::duration_cast<std::chrono::milliseconds>(end_exec - start_exec).count();
            updateDynamicMetrics(key, obs, 0);

            // Step 4: Final Cleanup & Telemetry
            j.finish_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
            
            SystemSnapshot snap = SystemMonitor::takeSnapshot();
            logger->logJob(j, rc, snap);

            std::ofstream sys_out(sys.sys_file, std::ios::app);
            if(sys_out.is_open()) {
                sys_out << j.finish_ts << ";" << snap.cpu_load << ";" << snap.mem_used_gb << "\n";
            }

            rm->releaseCores(j.assigned_cores);
            rm->releasePort(j.assigned_port);
            try {
                fs::remove(sys.snni_dir + "/" + j.assigned_file + "_server.dat");
                fs::remove(sys.snni_dir + "/" + j.assigned_file + "_client.dat");
            } catch (...) {}
            fm->releaseFile(key, j.assigned_file);

            static std::atomic<int> completed{0};
            if (++completed % 10 == 0) saveDynamicProfile();

        }).detach();
    }
}

void SAPPISServer::updateDynamicMetrics(const std::string& key, long observed_inf, [[maybe_unused]] long observed_pre) {
    if (profiles.find(key) == profiles.end()) return;
    auto& prof = profiles.at(key);
    const double alpha = 0.2; 
    if (observed_inf > 0) {
        long current = prof.dynamic_inf_ms.load();
        long next = (long)((alpha * observed_inf) + ((1.0 - alpha) * current));
        prof.dynamic_inf_ms.store(next);
        std::cout << "[EMA] " << key << " updated: " << next << "ms" << std::endl;
    }
}

void SAPPISServer::saveDynamicProfile() {
    std::lock_guard<std::mutex> lock(mtx_profiles);
    std::string path = sys.dynamic_profile_path.empty() ? "logs/dynamic_profile.cfg" : sys.dynamic_profile_path;
    std::ofstream out(path);
    if (!out.is_open()) return;
    out << "# model, batch, pre_static, inf_static, threads, max_buff, file_mb, pre_mem, inf_mem, DYNAMIC_INF_AVG\n";
    for (auto const& [key, p] : profiles) {
        out << p.model << ", " << p.batch << ", " << p.preproc_ms << ", " << p.inference_ms << ", "
            << p.threads << ", " << p.max_buffer << ", " << p.file_size_mb << ", " 
            << p.pre_mem_mb << ", " << p.inf_mem_mb << ", " << p.dynamic_inf_ms.load() << "\n";
    }
}