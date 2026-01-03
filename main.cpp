#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstring>

#include "ConfigManager.hpp"
#include "ResourceManager.hpp"
#include "FileManager.hpp"
#include "FCFSScheduler.hpp"
#include "StringUtils.hpp"
#include "ProcessLauncher.hpp"
#include "Logger.hpp"

// --- Global State ---
std::atomic<bool> running{true};
int server_fd_global = -1;
std::string global_server_ip = "127.0.0.1";

// --- Signal Handling ---
void signalHandler(int signum) {
    std::cout << "\n[Server] Shutdown signal received (" << signum << "). Cleaning up..." << std::endl;
    running = false;
    if (server_fd_global != -1) {
        // Force break the accept() call
        shutdown(server_fd_global, SHUT_RDWR);
        close(server_fd_global);
    }
}


long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * Dispatcher Thread:
 * - Monitors the job queue.
 * - Synchronizes File access and CPU core availability.
 * - Notifies the client using the SERVER_IP defined in config.cfg.
 * - Spawns the SHARK Server process with a model-specific timeout.
 */
void dispatcherThread(SystemConfig& sys, std::map<std::string, ModelProfile>& profiles, 
                      IScheduler& scheduler, FileManager& fm, ResourceManager& rm, Logger& logger) {
    while (running) {
        auto job_opt = scheduler.pop();
        if (!job_opt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        Job j = *job_opt;
        std::string key = j.model + "_" + std::to_string(j.batch);

        if (profiles.find(key) == profiles.end()) {
            std::cerr << "[Dispatcher] Error: Profile not found for " << key << std::endl;
            close(j.client_sock);
            continue;
        }

        ModelProfile& prof = profiles[key];
        int req_threads = prof.threads;
        // Timeout calculation: 2x expected time + 10s buffer
        int timeout_sec = (prof.inference_ms / 1000) * 2 + 10;

        std::string file_prefix = "";
        
        // --- Resource Handshake Loop ---
        // We must have BOTH a Ready file AND free CPU cores before we notify the client.
        while (running) {
            file_prefix = fm.acquireFile(key);
            if (!file_prefix.empty()) {
                if (rm.tryAcquireCores(req_threads)) {
                    break; // Success: Resources secured
                } else {
                    // Have file but no cores: Return file to READY state for others
                    fm.setReady(key, file_prefix); 
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            } else {
                // No file ready yet: Wait for the Replenisher/Dealer
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }

        if (!running) break;

        // --- Hardware Assignment ---
        int port = rm.acquirePort();
        j.start_ts = nowMs();

        // --- Step 2: Notify Client (Dispatch) ---
        // We use sys.server_ip from config.cfg to ensure the client connects to the right interface
        std::string target_ip = sys.server_ip.empty() ? "127.0.0.1" : sys.server_ip;
        
        std::stringstream ss;
        ss << "START_INF:" << target_ip << ":" << port << ":" 
           << j.model << ":" << j.batch << ":" << file_prefix;
        std::string msg = ss.str();
        
        std::cout << "[Dispatcher] Dispatching " << key << " to client at " << target_ip << ":" << port << std::endl;
        
        // Send the dispatch message and immediately close the management socket
        send(j.client_sock, msg.c_str(), msg.length(), 0);
        close(j.client_sock); 

        // --- Step 3: Execute SHARK Server ---
        std::string cmd = StringUtils::buildCommand(sys.server_cmd_template, sys.snni_dir, 
                                                   j.model, j.batch, port, file_prefix, target_ip);
        
        int rc = ProcessLauncher::runShellCommand(cmd, timeout_sec);

        // --- Cleanup ---
        j.finish_ts = nowMs();
        rm.releaseCores(req_threads);
        rm.releasePort(port);
        fm.releaseFile(key, file_prefix); // Free up slot in the buffer of N

        logger.logJob(j, rc);
        std::cout << "[Dispatcher] Job " << key << " finished. RC=" << rc << std::endl;
    }
}
int main() {
    signal(SIGINT, signalHandler);

    try {
        
        // 1. Initial Load and IP Detection
        SystemConfig sys = ConfigManager::loadSystemConfig("config.cfg");
        auto profiles = ConfigManager::loadProfiles("profile.cfg");
        
        ResourceManager res_mgr(sys.total_cores, sys.base_port, sys.port_range, sys.max_preproc_concurrency);
        FileManager file_mgr;
        FCFSScheduler scheduler;
        Logger logger(sys.log_file);

        // 2. Start Dispatcher Thread
        std::thread dispatch_thread(dispatcherThread, std::ref(sys), std::ref(profiles), 
                                    std::ref(scheduler), std::ref(file_mgr), std::ref(res_mgr), std::ref(logger));

        // 3. Start Proactive Replenisher Thread
        std::thread replenisher([&]() {
            while (running) {
                for (auto const& [key, p] : profiles) {
                    if (!running) break;
                    if (file_mgr.getActiveCount(key) < p.max_buffer) {
                        if (res_mgr.tryAcquirePreproc()) {
                            std::string prefix = file_mgr.initiateFile(key);
                            std::string cmd = StringUtils::buildCommand(sys.preproc_cmd_template, sys.snni_dir, 
                                                                        p.model, p.batch, 0, prefix);
                            int preproc_timeout = (p.preproc_ms / 1000) * 2 + 30;

                            std::thread([&, cmd, key, prefix, preproc_timeout]() {
                                ProcessLauncher::runShellCommand(cmd, preproc_timeout);
                                file_mgr.setReady(key, prefix);
                                res_mgr.releasePreproc(); 
                            }).detach();
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(1)); 
            }
        });

        // 4. Setup Main Listener Socket
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        server_fd_global = server_fd;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{AF_INET, htons((uint16_t)sys.scheduler_port), {INADDR_ANY}};
        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) throw std::runtime_error("Bind failed");
        listen(server_fd, sys.max_conn);

        std::cout << "[SAPPIS] Listening for requests on " << global_server_ip << ":" << sys.scheduler_port << std::endl;

        // 5. Admission Control Loop
        while (running) {
            sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int c_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
            
            if (c_sock < 0) {
                if (running) perror("accept");
                continue;
            }

            char buf[1024];
            memset(buf, 0, 1024);
            int valread = read(c_sock, buf, 1023);
            if (valread <= 0) { close(c_sock); continue; }
            buf[valread] = '\0'; // Ensure string safety

            std::string req(buf);
            // Clean string: remove any commas or trailing carriage returns from client
            req.erase(std::remove(req.begin(), req.end(), ','), req.end());
            req.erase(std::remove(req.begin(), req.end(), '\r'), req.end());
            req.erase(std::remove(req.begin(), req.end(), '\n'), req.end());

            std::cout << "[Server] Admission Request: " << req << std::endl;
            
            // Format: r_model_batch_slo
            std::stringstream ss(req);
            std::string part;
            std::vector<std::string> parts;
            while(std::getline(ss, part, '_')) {
                if(!part.empty()) parts.push_back(part);
            }

            if (parts.size() < 4) {
                send(c_sock, "REJECTED", 8, 0);
                close(c_sock);
                continue;
            }

            std::string model = parts[1];
            int batch = std::stoi(parts[2]);
            long slo = std::stol(parts[3]);
            std::string key = model + "_" + std::to_string(batch);

            bool admitted = false;
            if (profiles.count(key)) {
                // Basic Admission check using K-factor
                if (slo >= profiles[key].inference_ms * sys.default_slo_k_factor) admitted = true;
            }

            if (admitted) {
                send(c_sock, "ACCEPTED", 8, 0);
                Job j; j.type = 'r'; j.client_sock = c_sock; j.model = model; j.batch = batch;
                j.arrival_ts = nowMs(); j.requested_slo_ms = slo;
                scheduler.push(j);
            } else {
                send(c_sock, "REJECTED", 8, 0);
                close(c_sock);
            }
        }

        if (dispatch_thread.joinable()) dispatch_thread.join();
        if (replenisher.joinable()) replenisher.join();

    } catch (const std::exception& e) {
        if (running) std::cerr << "FATAL SERVER ERROR: " << e.what() << std::endl;
    }
    return 0;
}