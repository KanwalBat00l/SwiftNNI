#include "SwiftServer.hpp"
#include "ProcessRunner.hpp"
#include "SchedulerFactory.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstring>

static long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

SwiftServer::SwiftServer(SystemConfig config, std::map<std::string, ModelProfile> model_profiles)
    : sys(config), profiles(model_profiles) {

    // SwiftNNI ResourceManager only needs to track threads and ports
    rm = std::make_unique<ResourceManager>(sys.total_cores, sys.base_port, sys.port_range,
                                           sys.max_preproc_concurrency);
    fm = std::make_unique<FileManager>();
    logger = std::make_unique<Logger>(sys.log_file);

    // Create Scheduler (FCFS or SJF) via Factory
    scheduler = SchedulerFactory::create(sys.scheduler_mode, sys.aging_factor);
}

void SwiftServer::start() {
    running = true;
    std::cout << "[SwiftServer] Starting threads..." << std::endl;

    std::thread d_thread(&SwiftServer::dispatcherLoop, this);
    std::thread r_thread(&SwiftServer::replenisherLoop, this);

    listenerLoop(); // Main thread blocks here

    if (d_thread.joinable()) d_thread.join();
    if (r_thread.joinable()) r_thread.join();
}

void SwiftServer::stop() {
    running = false;
    if (server_fd != -1) { 
        shutdown(server_fd, SHUT_RDWR); 
        close(server_fd); 
    }
}

/**
 * LISTENER: Simple Admission Control.
 * Receives: r_model_batch or a_model_batch
 */
void SwiftServer::listenerLoop() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(sys.scheduler_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("Bind failed on port " + std::to_string(sys.scheduler_port));
    }
    listen(server_fd, sys.max_conn);

    std::cout << "[Listener] Listening on port " << sys.scheduler_port << std::endl;

    while (running) {
        sockaddr_in c_addr;
        socklen_t addr_len = sizeof(c_addr);
        int c_sock = accept(server_fd, (struct sockaddr*)&c_addr, &addr_len);
        if (c_sock < 0) continue;

        char buf[256] = {0};
        if (read(c_sock, buf, 255) <= 0) { close(c_sock); continue; }

        std::string req(buf);
        // Format: <type>_<model>_<batch>
        std::stringstream ss(req);
        std::string type_str, model, batch_str;
        std::getline(ss, type_str, '_');
        std::getline(ss, model, '_');
        std::getline(ss, batch_str, '_');

        std::string key = model + "_" + batch_str;

        // 1. Validate Model exists in profile
        if (profiles.find(key) == profiles.end()) {
            send(c_sock, "REJECTED_INVALID_MODEL", 22, 0);
            close(c_sock);
            continue;
        }

        // 2. Accept and Enqueue
        send(c_sock, "ACCEPTED", 8, 0);
        
        Job j;
        j.type = type_str[0]; 
        j.client_sock = c_sock;
        j.model = model;
        j.batch = std::stoi(batch_str);
        j.arrival_ts = nowMs();
        j.est_inf_ms = profiles[key].inf_ms;
        j.est_pre_ms = profiles[key].pre_ms;

        scheduler->push(j);
        std::cout << "[Listener] Enqueued: " << key << " (" << j.type << ")" << std::endl;
    }
}

/**
 * DISPATCHER: Resource-aware job execution.
 */
void SwiftServer::dispatcherLoop() {
    while (running) {
        // popReadyJob handles: 1. Ranking (SJF/FCFS) 2. File Availability Check
        auto job_opt = scheduler->popReadyJob(*fm, profiles);
        
        if (!job_opt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        Job j = *job_opt;
        std::string key = j.model + "_" + std::to_string(j.batch);
        int threads_needed = profiles[key].threads;

        // Check if we have enough threads
        if (!rm->acquireThreads(threads_needed)) {
            // Not enough threads available, put back in queue (Bypass)
            scheduler->push(j);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Setup execution
        j.assigned_threads = threads_needed;
        j.assigned_port = rm->acquirePort();
        j.start_ts = nowMs();

        // Calculate dynamic timeout: (est_ms * factor / 1000)
        int timeout_s = std::max(sys.min_timeout_s, 
                        (int)((j.est_inf_ms * sys.timeout_factor_inf) / 1000));

        // Build Mode 0 Command
        // Template: ./benchmark-{MODEL} 0 {SERVER_IP} {PORT} {BATCH} {FILE}
        std::string cmd = sys.server_cmd_template;
        auto replace = [&](std::string anchor, std::string val) {
            size_t pos; while((pos = cmd.find(anchor)) != std::string::npos) cmd.replace(pos, anchor.length(), val);
        };
        replace("{MODEL}", j.model);
        replace("{BATCH}", std::to_string(j.batch));
        replace("{PORT}", std::to_string(j.assigned_port));
        replace("{SERVER_IP}", sys.server_ip);
        replace("{FILE}", j.assigned_file);
        j.cmd = cmd;

        std::thread([this, j, timeout_s]() mutable {
            // Notify Client of where to connect
            std::string msg = "PORT:" + std::to_string(j.assigned_port) + ";THREADS:" + std::to_string(j.assigned_threads);
            send(j.client_sock, msg.c_str(), msg.length(), 0);
            close(j.client_sock);

            // Run process
            CmdResult res = ProcessRunner::run(j, sys.snni_dir, timeout_s);

            // Cleanup
            j.finish_ts = nowMs();
            j.exit_code = res.rc;
            
            rm->releaseThreads(j.assigned_threads);
            rm->releasePort(j.assigned_port);
            fm->deleteFile(j.assigned_file, sys.snni_dir); // Single-use deletion
            
            logger->logJob(j); // Simple CSV log
        }).detach();
    }
}

/**
 * REPLENISHER: Manages single-use pre-processed files.
 */
void SwiftServer::replenisherLoop() {
    while (running) {
        for (auto& [key, p] : profiles) {
            // Trigger pre-proc if status is DIRTY and we have room in pre-proc pool
            if (fm->getStatus(key) == FileStatus::DIRTY && rm->hasPreprocSlot()) {
                
                std::string filename = fm->initiatePreproc(key);
                rm->occupyPreprocSlot();

                int timeout_s = std::max(sys.min_timeout_s, 
                                (int)((p.pre_ms * sys.timeout_factor_pre) / 1000));

                // Build Mode 2 Command
                std::string cmd = sys.preproc_cmd_template;
                auto replace = [&](std::string anchor, std::string val) {
                    size_t pos; while((pos = cmd.find(anchor)) != std::string::npos) cmd.replace(pos, anchor.length(), val);
                };
                replace("{MODEL}", p.model);
                replace("{BATCH}", std::to_string(p.batch));
                replace("{PORT}", std::to_string(sys.pre_base_port)); // Generic pre-proc port
                replace("{FILE}", filename);

                std::thread([this, cmd, key, filename, timeout_s]() {
                    Job pj; 
                    pj.assigned_threads = 1; // Pre-proc usually single-threaded
                    pj.cmd = cmd;

                    CmdResult res = ProcessRunner::run(pj, sys.snni_dir, timeout_s);

                    if (res.rc == 0) {
                        fm->setReady(key, filename);
                    } else {
                        // If failed, fm status remains GENERATING or reset to DIRTY logic
                        // For SwiftNNI, we reset to dirty on failure to try again
                        // (Handled by initiatePreproc implicitly)
                    }
                    rm->releasePreprocSlot();
                }).detach();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}