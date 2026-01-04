#include "SAPPISServer.hpp"
#include "StringUtils.hpp"
#include "ProcessLauncher.hpp"
#include "SystemMonitor.hpp"
#include "ConfigManager.hpp"

// Concrete Scheduler Implementations
//#include "FCFSScheduler.hpp" // NEW: Required for make_unique
//#include "SJFScheduler.hpp"  // NEW: Required for make_unique

#include "SchedulerFactory.hpp" // Header for the new factory


#include <sys/socket.h>
#include <netinet/in.h>
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
 * Helper: Get current system time in milliseconds since epoch.
 */
 static long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

SAPPISServer::SAPPISServer(SystemConfig config, std::map<std::string, ModelProfile> model_profiles)
    : sys(config), profiles(model_profiles) {
    
    // Initialize Resource, File, and Log Managers
    rm = std::make_unique<ResourceManager>(sys.total_cores, sys.base_port, sys.port_range, 
                                           sys.max_preproc_concurrency, sys.enable_core_pinning);
    fm = std::make_unique<FileManager>();
    logger = std::make_unique<Logger>(sys.log_file);

    /**
     * FACTORY PATTERN:
     * Instead of hardcoding FCFSScheduler or SJFScheduler here, we delegate
     * the creation to the Factory. This makes extending the system easy.
     */
    scheduler = SchedulerFactory::create(sys.scheduler_mode, sys.aging_factor);
}


void SAPPISServer::start() {
    running = true;
    std::cout << "[Server] Engine starting in " << sys.scheduler_mode << " mode..." << std::endl;

    // Launch background threads for Task Dispatching and File Pre-processing
    std::thread d_thread(&SAPPISServer::dispatcherLoop, this);
    std::thread r_thread(&SAPPISServer::replenisherLoop, this);

    // Main thread enters the Listener Loop (Admission Control)
    listenerLoop();

    if (d_thread.joinable()) d_thread.join();
    if (r_thread.joinable()) r_thread.join();
}

void SAPPISServer::stop() {
    running = false;
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
    }
}



/**
 * REPLENISHER LOOP: Proactively ensures the buffer of .dat files is full.
 */
void SAPPISServer::replenisherLoop() {
    while (running) {
        for (auto& [key, p] : profiles) {
            if (!running) break;
            
            // Check if we need more files for this model-batch
            if (fm->getActiveCount(key) < p.max_buffer) {
                if (rm->hasCapacityForDealer()) {
                    std::string prefix = fm->initiateFile(key);
                    
                    // Dealer runs on a single core from the Dealer Pool
                    int dealer_core = rm->acquireDealerCore(); 
                    std::string d_core_str = (dealer_core != -1) ? std::to_string(dealer_core) : "";

                    std::string cmd = StringUtils::buildCommand(sys.preproc_cmd_template, sys.snni_dir, 
                                                               p.model, p.batch, 0, prefix, "127.0.0.1", d_core_str);

                    // Detach thread to run Dealer binary without blocking the Replenisher
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
 * LISTENER LOOP: The system's "Admission Controller".
 * 
 * Steps:
 * 1. Initialize the master TCP socket.
 * 2. Bind to the configured SAPPIS port.
 * 3. Listen for incoming client requests.
 * 4. Parse the request and perform DYNAMIC admission control based on 
 *    the current moving average of inference latency.
 */
void SAPPISServer::listenerLoop() {
    // 1. Create the socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("Listener: Could not create socket");
    }

    // Allow immediate reuse of the port after server restart
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Setup address structure
    // We use memset to ensure the entire struct (including padding) is zeroed.
    // This prevents "uninitialized field" warnings and potential network bugs.
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(sys.scheduler_port));
    addr.sin_addr.s_addr = INADDR_ANY;

    // 3. Bind and Listen
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        throw std::runtime_error("Listener: Bind failed on port " + std::to_string(sys.scheduler_port));
    }

    if (listen(server_fd, sys.max_conn) < 0) {
        close(server_fd);
        throw std::runtime_error("Listener: Listen failed");
    }

    std::cout << "[Listener] SAPPIS Engine accepting requests on port " << sys.scheduler_port << std::endl;

    while (running) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        // Block until a client connects
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            if (running) perror("[Listener] accept error");
            continue;
        }

        // 4. Read the Request (Expected format: r_model_batch_slo)
        char buffer[1024] = {0};
        ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer) - 1);
        
        if (bytes_read <= 0) {
            close(client_sock);
            continue;
        }

        // Clean string and split by underscores
        std::string request_str = ConfigManager::trim(buffer);
        std::stringstream ss(request_str);
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(ss, part, '_')) {
            if (!part.empty()) parts.push_back(part);
        }

        // Basic validation
        if (parts.size() < 4) {
            std::cerr << "[Listener] Malformed request: " << request_str << std::endl;
            send(client_sock, "REJECTED_FORMAT", 15, 0);
            close(client_sock);
            continue;
        }

        std::string model_name = parts[1];
        int batch_size = std::stoi(parts[2]);
        long requested_slo = std::stol(parts[3]);
        std::string profile_key = model_name + "_" + std::to_string(batch_size);

        // 5. Admission Control
        if (profiles.find(profile_key) == profiles.end()) {
            send(client_sock, "REJECTED_UNKNOWN_MODEL", 22, 0);
            close(client_sock);
            continue;
        }

        /**
         * DYNAMIC ADMISSION CHECK:
         * We retrieve the current Moving Average of the inference time.
         * If the system is under load and latency has spiked, 'expected_ms'
         * will be higher, causing tighter SLOs to be rejected automatically.
         */
        long expected_ms = profiles.at(profile_key).dynamic_inf_ms.load();
        long feasible_threshold = static_cast<long>(expected_ms * sys.default_slo_k_factor);

        if (requested_slo >= feasible_threshold) {
            // ACCEPTED: Construct the Job and move it to the scheduler
            Job new_job;
            new_job.type = 'r';
            new_job.client_sock = client_sock;
            new_job.model = model_name;
            new_job.batch = batch_size;
            new_job.arrival_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count();
            new_job.requested_slo_ms = requested_slo;

            send(client_sock, "ACCEPTED", 8, 0);
            scheduler->push(new_job);
            
            std::cout << "[Admission] Job " << profile_key << " Accepted (Expected: " << expected_ms << "ms)" << std::endl;
        } else {
            // REJECTED: Inform client and close connection
            std::cout << "[Admission] Job " << profile_key << " Rejected (SLO " << requested_slo 
                      << "ms < Threshold " << feasible_threshold << "ms)" << std::endl;
            send(client_sock, "REJECTED_SLO_UNFEASIBLE", 23, 0);
            close(client_sock);
        }
    }
}


/**
 * DISPATCHER LOOP: Synchronizes files and hardware, then executes.
 */
void SAPPISServer::dispatcherLoop() {
    while (running) {
        auto job_opt = scheduler->pop();
        if (!job_opt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        Job j = *job_opt;
        std::string key = j.model + "_" + std::to_string(j.batch);
        ModelProfile& prof = profiles.at(key);

        // --- STEP 1: Resource Synchronization ---
        while (running) {
            j.assigned_file = fm->acquireFile(key);
            if (!j.assigned_file.empty()) {
                j.assigned_cores = rm->acquireInferenceCores(prof.threads);
                if (!j.assigned_cores.empty()) break; 
                fm->setReady(key, j.assigned_file);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!running) break;

        // --- STEP 2: Timing & Dispatch ---
        j.assigned_port = rm->acquirePort();
        j.start_ts = nowMs(); // Utilizing the nowMs() helper

        std::string core_str = rm->coresToString(j.assigned_cores);
        std::string cmd = StringUtils::buildCommand(sys.server_cmd_template, sys.snni_dir, 
                                                   j.model, j.batch, j.assigned_port, 
                                                   j.assigned_file, sys.server_ip, core_str);

        // Notify client and close socket
        std::stringstream ss;
        ss << "START_INF:" << sys.server_ip << ":" << j.assigned_port << ":" 
           << j.model << ":" << j.batch << ":" << j.assigned_file;
        send(j.client_sock, ss.str().c_str(), ss.str().length(), 0);
        close(j.client_sock);

        // --- STEP 3: Safety Execution ---
        // Safety timeout set to 5x expected time to catch deadlocks
        long expected_inf = prof.dynamic_inf_ms.load();
        int safety_timeout = static_cast<int>((expected_inf / 1000) * 5 + 60);
        if (safety_timeout < 120) safety_timeout = 120;

        auto start_exec = std::chrono::steady_clock::now();
        int rc = ProcessLauncher::runShellCommand(cmd, safety_timeout);
        auto end_exec = std::chrono::steady_clock::now();

        // --- STEP 4: Profile Update & Logging ---
        long observed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_exec - start_exec).count();
        updateDynamicMetrics(key, observed_ms, 0);

        j.finish_ts = nowMs(); // Utilizing the nowMs() helper
        SystemSnapshot snap = SystemMonitor::takeSnapshot();
        logger->logJob(j, rc, snap);

        rm->releaseCores(j.assigned_cores);
        rm->releasePort(j.assigned_port);
        
        try {
            fs::remove(sys.snni_dir + "/" + j.assigned_file + "_server.dat");
            fs::remove(sys.snni_dir + "/" + j.assigned_file + "_client.dat");
        } catch (...) {}
        
        fm->releaseFile(key, j.assigned_file);
    }
}

/**
 * DYNAMIC PROFILING: Updates the moving average of execution times.
 * FIX: Added [[maybe_unused]] to observed_pre.
 */
void SAPPISServer::updateDynamicMetrics(const std::string& key, 
                                        long observed_inf, 
                                        [[maybe_unused]] long observed_pre) {
    if (profiles.find(key) == profiles.end()) return;
    
    auto& prof = profiles.at(key);
    const double alpha = 0.2; 

    if (observed_inf > 0) {
        long current = prof.dynamic_inf_ms.load();
        long next = (long)((alpha * observed_inf) + ((1.0 - alpha) * current));
        prof.dynamic_inf_ms.store(next);
        std::cout << "[Dynamic] " << key << " updated to " << next << "ms" << std::endl;
    }
}