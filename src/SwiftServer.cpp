#include "SwiftServer.hpp"
#include "StringUtils.hpp"
#include "ProcessRunner.hpp"
#include "SchedulerFactory.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstring>
#include <filesystem>
namespace fs = std::filesystem;

static long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

SwiftServer::SwiftServer(SystemConfig config, std::map<std::string, ModelProfile> model_profiles)
    : sys(config), profiles(model_profiles) {
    
    try {
        fs::path log_path(sys.log_file);
        if (log_path.has_parent_path()) {
            fs::create_directories(log_path.parent_path());
        }
    } catch (const std::exception& e) {
        std::cerr << "[Swift] Error creating log directory: " << e.what() << std::endl;
    }
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

void SwiftServer::dispatcherLoop() {
    while (running) {
        auto job_opt = scheduler->popReadyJob(*fm, profiles);
        
        if (!job_opt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        Job j = *job_opt;
        std::string key = j.model + "_" + std::to_string(j.batch);
        int threads_needed = profiles[key].threads;

        if (!rm->acquireThreads(threads_needed)) {
            scheduler->push(j);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        j.assigned_threads = threads_needed;
        j.assigned_port = rm->acquirePort();
        j.start_ts = nowMs();

        int timeout_s = std::max(sys.min_timeout_s, 
                        (int)((j.est_inf_ms * sys.timeout_factor_inf) / 1000));

        // Build the handshake message including the unique filename
        std::string handshake = "PORT:" + std::to_string(j.assigned_port) + 
                                ";THREADS:" + std::to_string(j.assigned_threads) + 
                                ";FILE:" + j.assigned_file + ";";

        // Dispatch the worker thread
        std::thread([this, j, timeout_s, handshake]() mutable {
            // 1. Send handshake to client
            send(j.client_sock, handshake.c_str(), handshake.length(), 0);
            close(j.client_sock);

            // 2. Build Mode 0 Command
            j.cmd = StringUtils::buildCommand(
                sys.server_cmd_template, sys.snni_dir,
                j.model, j.batch, j.assigned_port,
                j.assigned_file, sys.server_ip, j.assigned_threads
            );

            // 3. Run inference process
            CmdResult res = ProcessRunner::run(j, sys.snni_dir, timeout_s);

            // 4. Cleanup
            j.finish_ts = nowMs();
            j.exit_code = res.rc;
            
            rm->releaseThreads(j.assigned_threads);
            rm->releasePort(j.assigned_port);
            fm->deleteFile(j.assigned_file, sys.snni_dir); 
            
            logger->logJob(j);
        }).detach();
    }
} 


void SwiftServer::replenisherLoop() {
    static std::atomic<int> preproc_port_counter{0};

    while (running) {
        for (auto& [key, p] : profiles) {
            if (fm->getStatus(key) == FileStatus::DIRTY && rm->hasPreprocSlot()) {
                
                std::string filename = fm->initiatePreproc(key);
                rm->occupyPreprocSlot();

                int timeout_s = std::max(sys.min_timeout_s, 
                                (int)((p.pre_ms * sys.timeout_factor_pre) / 1000));

                int preproc_port = sys.pre_base_port + (preproc_port_counter.fetch_add(1) % 1000);

                std::string cmd = StringUtils::buildCommand(
                    sys.preproc_cmd_template, sys.snni_dir,
                    p.model, p.batch, preproc_port, filename, 
                    sys.server_ip, 1
                );

                std::thread([this, cmd, key, filename, timeout_s]() {
                    Job pj; 
                    pj.assigned_threads = 1;
                    pj.cmd = cmd;

                    CmdResult res = ProcessRunner::run(pj, sys.snni_dir, timeout_s);

                    if (res.rc == 0) {
                        fm->setReady(key, filename);
                    } else {
                        // On failure, status remains GENERATING; 
                        // You could reset to DIRTY here if you want immediate retry.
                    }
                    rm->releasePreprocSlot();
                }).detach();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}