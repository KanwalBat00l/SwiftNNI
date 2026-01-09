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

namespace fs = std::filesystem;

static long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

SAPPISServer::SAPPISServer(SystemConfig config, std::map<std::string, ModelProfile> model_profiles)
    : sys(config), profiles(model_profiles) {
    rm = std::make_unique<ResourceManager>(sys.total_cores, sys.base_port, sys.port_range, 
                                           sys.max_preproc_concurrency, sys.enable_core_pinning);
    fm = std::make_unique<FileManager>();
    logger = std::make_unique<Logger>(sys.log_file);
    job_tracker = std::make_unique<ActiveJobTracker>();
    scheduler = SchedulerFactory::create(sys.scheduler_mode, sys.aging_factor);
}

void SAPPISServer::start() {
    running = true;
    std::thread d_thread(&SAPPISServer::dispatcherLoop, this);
    std::thread r_thread(&SAPPISServer::replenisherLoop, this);
    listenerLoop();
    if (d_thread.joinable()) d_thread.join();
    if (r_thread.joinable()) r_thread.join();
}

void SAPPISServer::stop() {
    running = false;
    saveDynamicProfile();
    if (server_fd != -1) { shutdown(server_fd, SHUT_RDWR); close(server_fd); }
}

/**
 * LISTENER: Implementation of Virtual Finish Time (VFT) Admission Control.
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
        throw std::runtime_error("Listener: Bind failed on port " + std::to_string(sys.scheduler_port));
    }
    listen(server_fd, sys.max_conn);

    // Calculate system core capacity for VFT math
    int total_inf_cores = sys.total_cores - sys.system_reserved_cores - sys.max_preproc_concurrency;
    if (total_inf_cores <= 0) total_inf_cores = 1;

    while (running) {
        sockaddr_in c_addr;
        socklen_t addr_len = sizeof(c_addr);
        int c_sock = accept(server_fd, (struct sockaddr*)&c_addr, &addr_len);
        if (c_sock < 0) continue;

        char buf[1024] = {0};
        if (read(c_sock, buf, 1023) <= 0) { close(c_sock); continue; }

        std::string req = ConfigManager::trim(buf);
        std::stringstream ss(req);
        std::string part; std::vector<std::string> parts;
        while (std::getline(ss, part, '_')) if (!part.empty()) parts.push_back(part);

        if (parts.size() < 4) { close(c_sock); continue; }
        std::string key = parts[1] + "_" + parts[2];
        long requested_slo = std::stol(parts[3]);

        if (profiles.find(key) == profiles.end()) {
            send(c_sock, "REJECTED_MODEL", 14, 0);
            close(c_sock); continue;
        }

        // --- VIRTUAL FINISH TIME (VFT) ADMISSION CONTROL ---
        long v_queue = scheduler->getTotalWorkVolume(profiles);
        long v_running = job_tracker->getResidualWorkVolume();
        long expected_inf = profiles.at(key).dynamic_inf_ms.load();
        
        // EstWaitTime = Total Volume of Work / Processing Units (Cores)
        long est_wait = (v_queue + v_running) / total_inf_cores;
        long est_tat = est_wait + expected_inf;

        // Condition: Use Configurable Margin (e.g., 1.15 for 15% slack)
        if (requested_slo >= static_cast<long>(est_tat * sys.vft_safety_margin)) {
            send(c_sock, "ACCEPTED", 8, 0);
            Job j;
            j.type = 'r'; j.client_sock = c_sock; j.model = parts[1];
            j.batch = std::stoi(parts[2]); j.arrival_ts = nowMs();
            j.requested_slo_ms = requested_slo;
            scheduler->push(j);
        } else {
            // Inform the client of the current system delay for negotiation
            std::string msg = "REJECTED_VFT:SUGGESTED_SLO:" + std::to_string(static_cast<long>(est_tat * sys.vft_safety_margin));
            send(c_sock, msg.c_str(), msg.length(), 0);
            close(c_sock);
        }
    }
}
/**
 * DISPATCHER: Multi-threaded with Fault Isolation.
 */
 void SAPPISServer::dispatcherLoop() {
    SystemSnapshot snap = SystemMonitor::takeSnapshot();
    const double ram_limit = snap.total_mem_gb * 0.95;

    while (running) {
        auto job_opt = scheduler->popReadyJob(*fm, profiles, ram_limit);
        if (!job_opt) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); 
            continue; 
        }

        Job j = *job_opt;
        std::string key = j.model + "_" + std::to_string(j.batch);

        // Secure Resources
        j.assigned_cores = rm->acquireCoresElastic(profiles.at(key).threads);
        j.assigned_port = rm->acquirePort(); 
        j.start_ts = nowMs();

        // Track for VFT
        job_tracker->add(j.assigned_file, profiles.at(key).dynamic_inf_ms.load(), j.assigned_cores.size());

        // Dispatch Worker Thread
        std::thread([this, j, key]() mutable {
            // FIX: Access the model profile via reference
            ModelProfile& prof = profiles.at(key); 
            
            std::string core_str = rm->coresToString(j.assigned_cores);
            std::string cmd = StringUtils::buildCommand(sys.server_cmd_template, sys.snni_dir, 
                                                       j.model, j.batch, j.assigned_port, 
                                                       j.assigned_file, sys.server_ip, core_str);

            // Notify Client
            std::stringstream ss;
            ss << "START_INF:" << sys.server_ip << ":" << j.assigned_port << ":" << key << ":" << j.assigned_file;
            send(j.client_sock, ss.str().c_str(), ss.str().length(), 0);
            close(j.client_sock);

            // Execute with safety guard
            auto start_exec = std::chrono::steady_clock::now();
            int rc = ProcessLauncher::runShellCommand(cmd, 600); // 10 min cap
            auto end_exec = std::chrono::steady_clock::now();

            // Step 3: EMA Protection (Only learn from success)
            if (rc == 0) {
                long obs = std::chrono::duration_cast<std::chrono::milliseconds>(end_exec - start_exec).count();
                // Access dynamic metrics through 'prof' reference
                long cur = prof.dynamic_inf_ms.load();
                const double alpha = 0.2;
                prof.dynamic_inf_ms.store((long)((alpha * obs) + ((1.0 - alpha) * cur)));
            }

            // Step 4: Bookkeeping
            j.finish_ts = nowMs();
            SystemSnapshot s = SystemMonitor::takeSnapshot();
            logger->logJob(j, rc, s);

            std::ofstream sys_out(sys.sys_file, std::ios::app);
            if(sys_out.is_open()) sys_out << j.finish_ts << ";" << s.cpu_load << ";" << s.mem_used_gb << "\n";

            // Cleanup
            job_tracker->remove(j.assigned_file);
            rm->releaseCores(j.assigned_cores);
            rm->releasePort(j.assigned_port);
            fs::remove(sys.snni_dir + "/" + j.assigned_file + "_server.dat");
            fs::remove(sys.snni_dir + "/" + j.assigned_file + "_client.dat");
            fm->releaseFile(key, j.assigned_file);
            saveDynamicProfile();

        }).detach();
    }
}

void SAPPISServer::replenisherLoop() {
    while (running) {
        for (auto& [key, p] : profiles) {
            if (fm->getActiveCount(key) < p.max_buffer && rm->hasCapacityForDealer()) {
                std::string prefix = fm->initiateFile(key);
                int core = rm->acquireDealerCore(); 
                std::string cmd = StringUtils::buildCommand(sys.preproc_cmd_template, sys.snni_dir, 
                                                           p.model, p.batch, 0, prefix, "127.0.0.1", std::to_string(core));
                std::thread([this, cmd, key, prefix, p, core]() {
                    Job pj; pj.type = 'p'; pj.model = p.model; pj.batch = p.batch;
                    pj.assigned_file = prefix; pj.assigned_cores = {core};
                    pj.start_ts = nowMs();

                    int rc = ProcessLauncher::runShellCommand(cmd, 600);
                    
                    pj.finish_ts = nowMs();
                    logger->logJob(pj, rc, SystemMonitor::takeSnapshot()); // Dealer Bookkeeping

                    fm->setReady(key, prefix);
                    rm->releaseDealerSlot();
                    if (core != -1) rm->releaseCores({core});
                }).detach();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void SAPPISServer::updateDynamicMetrics(const std::string& key, long observed_inf, [[maybe_unused]] long observed_pre) {
    if (profiles.find(key) == profiles.end()) return;
    auto& prof = profiles.at(key);
    const double alpha = 0.2; 
    if (observed_inf > 0) {
        long cur = prof.dynamic_inf_ms.load();
        prof.dynamic_inf_ms.store((long)((alpha * observed_inf) + ((1.0 - alpha) * cur)));
    }
}

void SAPPISServer::saveDynamicProfile() {
    std::lock_guard<std::mutex> lock(mtx_profiles);
    std::string path = sys.dynamic_profile_path.empty() ? "logs/dynamic_profile.cfg" : sys.dynamic_profile_path;
    std::ofstream out(path);
    if (!out.is_open()) return;
    for (auto const& [key, p] : profiles) {
        out << p.model << ", " << p.batch << ", " << p.preproc_ms << ", " << p.inference_ms << ", "
            << p.threads << ", " << p.max_buffer << ", " << p.file_size_mb << ", " 
            << p.pre_mem_mb << ", " << p.inf_mem_mb << ", " << p.dynamic_inf_ms.load() << "\n";
    }
}