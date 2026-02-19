#include "SwiftServer.hpp"
#include "StringUtils.hpp"
#include "ProcessLauncher.hpp"
#include "SystemMonitor.hpp"
#include "ConfigManager.hpp"
#include "SchedulerFactory.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <filesystem>
#include <cstring>
#include <atomic>
#include <poll.h>

namespace fs = std::filesystem;

static long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

// Q-5: Global atomic request counter for unique request IDs
static std::atomic<long> g_req_counter{0};

/**
 * @brief Constructor for SwiftServer.
 * Initializes managers and uses the Factory to instantiate the chosen scheduler.
 */
SwiftServer::SwiftServer(SystemConfig config, std::map<std::string, ModelProfile> model_profiles)
    : sys(config), profiles(model_profiles) {

    // Initialize Resource, File, and Log Managers
    rm = std::make_unique<ResourceManager>(sys.total_cores, sys.base_port, sys.port_range,
                                           sys.max_preproc_concurrency, sys.enable_core_pinning);
    fm = std::make_unique<FileManager>();
    logger = std::make_unique<Logger>(sys.log_file);
    job_tracker = std::make_unique<ActiveJobTracker>();

    scheduler = SchedulerFactory::create(sys.scheduler_mode, sys.aging_factor, sys.dqn_weights_path,
                                           sys.sa_initial_temp, sys.sa_cooling_rate,
                                           sys.sa_max_iterations, sys.sa_window_size,
                                           sys.sa_min_retrigger_ms,
                                           sys.total_cores, sys.total_memory_mb);
}

/**
 * Q-2: TTFR Timer 1 — TCP Socket RTT Measurement.
 *
 * Sends a PING probe to the connected client and measures round-trip time.
 * θ_est = max(1.0, measured_rtt / baseline_rtt)
 *
 * Black-box approach: only uses the existing TCP connection, no SHARK internals.
 */
double SwiftServer::measureTTFR_Timer1(int client_sock) {
    auto t0 = std::chrono::steady_clock::now();

    // Send TTFR probe
    const char* probe = "TTFR_PING";
    if (send(client_sock, probe, 9, MSG_NOSIGNAL) <= 0) {
        return 1.0; // On error, assume symmetric (θ_c = 1.0)
    }

    // Wait for PONG with timeout
    struct pollfd pfd;
    pfd.fd = client_sock;
    pfd.events = POLLIN;
    int timeout_ms = 500; // 500ms timeout for RTT probe
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        return 1.0; // Timeout or error → assume symmetric
    }

    char pong[16] = {0};
    ssize_t n = recv(client_sock, pong, 15, 0);
    if (n <= 0) {
        return 1.0;
    }

    auto t1 = std::chrono::steady_clock::now();
    double rtt_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // θ_est = max(1.0, rtt / baseline)
    double theta = rtt_ms / sys.ttfr_baseline_rtt_ms;
    return std::max(1.0, theta);
}

/**
 * Q-3: Negotiation State Machine.
 *
 * When θ_c ≥ θ_threshold (default 1.5), proposes n_alt (lighter model) to client.
 * Protocol:
 *   Server → Client: "NEGOTIATE:<original_model>:<suggested_model>:<suggested_slo>"
 *   Client → Server: "ACCEPT" or "REFUSE" (within negotiation_timeout_ms)
 *
 * If client refuses, penalty_phi = 1 is applied (lower scheduling priority).
 * If client accepts, out_model/out_batch are updated to the alternative.
 */
bool SwiftServer::negotiateWithClient(int client_sock, const std::string& original_key,
                                       [[maybe_unused]] double theta_c,
                                       std::string& out_model, int& out_batch,
                                       int& out_penalty_phi) {
    out_penalty_phi = 0;

    // FU-6: Family-based N_ALT_MAP lookup — extract model family from original_key
    auto sep_orig = original_key.find('_');
    if (sep_orig == std::string::npos) return false;
    std::string original_family = original_key.substr(0, sep_orig);
    int original_batch = std::stoi(original_key.substr(sep_orig + 1));

    // Check if n_alt mapping exists for this model family
    if (sys.n_alt_map.find(original_family) == sys.n_alt_map.end()) {
        // No alternative models defined → skip negotiation, no penalty
        return false;
    }

    const auto& alternatives = sys.n_alt_map.at(original_family);
    if (alternatives.empty()) return false;

    // FU-6: Pick the first alternative family that exists in profiles (with same batch)
    std::string suggested_key;
    std::string suggested_model;
    int suggested_batch = original_batch;  // FU-6: Preserve batch size
    for (const auto& alt_family : alternatives) {
        std::string candidate_key = alt_family + "_" + std::to_string(original_batch);
        if (profiles.find(candidate_key) != profiles.end()) {
            suggested_key = candidate_key;
            suggested_model = alt_family;
            break;
        }
    }
    if (suggested_key.empty()) return false;

    // Compute suggested SLO based on alternative's expected time
    long alt_inf = profiles.at(suggested_key).dynamic_inf_ms.load();
    long suggested_slo = static_cast<long>(alt_inf * sys.slo_factor_tier2);

    // Send negotiation proposal
    std::stringstream msg;
    msg << "NEGOTIATE:" << original_key << ":" << suggested_key << ":" << suggested_slo;
    std::string msg_str = msg.str();
    if (send(client_sock, msg_str.c_str(), msg_str.length(), MSG_NOSIGNAL) <= 0) {
        return false;
    }

    // Wait for client response with timeout
    struct pollfd pfd;
    pfd.fd = client_sock;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, sys.negotiation_timeout_ms);
    if (ret <= 0) {
        // Timeout → client is slow, apply penalty and keep original model
        out_penalty_phi = 1;
        return false;
    }

    char response[32] = {0};
    ssize_t n = recv(client_sock, response, 31, 0);
    if (n <= 0) {
        out_penalty_phi = 1;
        return false;
    }

    std::string resp = ConfigManager::trim(std::string(response, n));
    if (resp == "ACCEPT") {
        // Client accepted the alternative model
        out_model = suggested_model;
        out_batch = suggested_batch;
        out_penalty_phi = 0;
        return true;
    } else {
        // Client refused → apply priority penalty φ=1
        out_penalty_phi = 1;
        return false;
    }
}

void SwiftServer::start() {
    running = true;
    throughput_start_ts = nowMs();
    std::thread d_thread(&SwiftServer::dispatcherLoop, this);
    std::thread r_thread(&SwiftServer::replenisherLoop, this);

    // Q-8: Start telemetry thread if enabled
    std::thread t_thread;
    if (sys.enable_time_series_logging) {
        t_thread = std::thread(&SwiftServer::telemetryLoop, this);
    }

    try {
        listenerLoop();
    } catch (...) {
        // Ensure threads are joined before rethrowing to avoid std::terminate
        running = false;
        if (d_thread.joinable()) d_thread.join();
        if (r_thread.joinable()) r_thread.join();
        if (t_thread.joinable()) t_thread.join();
        throw;
    }
    if (d_thread.joinable()) d_thread.join();
    if (r_thread.joinable()) r_thread.join();
    if (t_thread.joinable()) t_thread.join();
}

void SwiftServer::stop() {
    running = false;
    saveDynamicProfile();
    if (server_fd != -1) { shutdown(server_fd, SHUT_RDWR); close(server_fd); }
}

/**
 * LISTENER: Multi-tier SLO Admission Control (Q-1, Q-4).
 *
 * Q-1: Dual-format parser — 4-field (legacy) or 8-field (Cap_c).
 * Q-4: Tier 1 Practicality + Tier 3 VFT sequential filter.
 */
 void SwiftServer::listenerLoop() {
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

        // Q-1: Dual-format parser — accept 4-field (legacy) or 8-field (Cap_c)
        if (parts.size() != 4 && parts.size() != 8) {
            send(c_sock, "ERROR_400", 9, 0);
            close(c_sock); continue;
        }

        std::string key = parts[1] + "_" + parts[2];
        long requested_slo = std::stol(parts[3]);

        // Q-1: Parse Cap_c fields or apply symmetric defaults
        int device_class = 4;      // Desktop
        float cpu_freq = 3.0f;
        int ram_mb = 8192;
        int net_type = 0;          // LAN
        bool is_default_cap = true;

        if (parts.size() == 8) {
            device_class = std::stoi(parts[4]);
            cpu_freq = std::stof(parts[5]);
            ram_mb = std::stoi(parts[6]);
            net_type = std::stoi(parts[7]);
            is_default_cap = false;
        }

        if (profiles.find(key) == profiles.end()) {
            send(c_sock, "REJECTED_MODEL", 14, 0);
            close(c_sock); continue;
        }

        // Q-2: TTFR Timer 1 — measure TCP RTT to estimate θ_c
        double theta_est = measureTTFR_Timer1(c_sock);

        // FU-9: Recalculate θ_est using per-network-type baseline
        {
            double baseline_for_net;
            switch (net_type) {
                case 0:  baseline_for_net = sys.ttfr_baseline_lan_ms;  break;
                case 1:  baseline_for_net = sys.ttfr_baseline_wifi_ms; break;
                case 2:  baseline_for_net = sys.ttfr_baseline_5g_ms;   break;
                case 3:  baseline_for_net = sys.ttfr_baseline_wan_ms;  break;
                default: baseline_for_net = sys.ttfr_baseline_rtt_ms;  break;
            }
            // Re-compute θ using the raw RTT and the differentiated baseline
            // θ_est was computed as rtt/baseline_default; scale to net-type baseline
            double raw_rtt_ms = theta_est * sys.ttfr_baseline_rtt_ms;
            theta_est = std::max(1.0, raw_rtt_ms / baseline_for_net);
        }

        // Q-4 Tier 1: Practicality Filter — reject if SLO < baseline e_on
        long baseline_inf = profiles.at(key).inference_ms;
        if (requested_slo < baseline_inf) {
            send(c_sock, "REJECTED_403:PRACTICALITY", 25, 0);
            close(c_sock); continue;
        }

        // Q-4 Tier 2: Static SLO Filter — reject if D < SLO_factor × E_{i,c}
        // E_{i,c} = e_pre + (θ_c × e_on) per Q-10 asymmetric burst time
        long dynamic_inf = profiles.at(key).dynamic_inf_ms.load();
        long dynamic_pre = profiles.at(key).dynamic_pre_ms.load();
        double e_ic = (double)dynamic_pre + (theta_est * (double)dynamic_inf);
        long tier2_threshold = static_cast<long>(sys.slo_factor_tier2 * e_ic);
        if (requested_slo < tier2_threshold) {
            send(c_sock, "REJECTED_403:STATIC_SLO", 23, 0);
            close(c_sock); continue;
        }

        // Q-3/FU-7: Negotiation State Machine — trigger if θ_c ≥ threshold
        std::string final_model = parts[1];
        int final_batch = std::stoi(parts[2]);
        int penalty_phi = 0;
        std::string original_model_key; // track if negotiation occurred

        if (theta_est >= sys.theta_negotiation_threshold) {
            // FU-6/FU-7: Extract model family for n_alt lookup
            std::string model_family = parts[1];

            if (sys.n_alt_map.find(model_family) != sys.n_alt_map.end()) {
                // Has alternatives → full negotiation handshake
                std::string neg_model;
                int neg_batch = 0;
                int neg_penalty = 0;
                bool accepted = negotiateWithClient(c_sock, key, theta_est,
                                                    neg_model, neg_batch, neg_penalty);
                if (accepted) {
                    original_model_key = key;
                    final_model = neg_model;
                    final_batch = neg_batch;
                    key = final_model + "_" + std::to_string(final_batch);
                    // Recalculate with new model
                    dynamic_inf = profiles.at(key).dynamic_inf_ms.load();
                    dynamic_pre = profiles.at(key).dynamic_pre_ms.load();
                }
                penalty_phi = neg_penalty;
            } else {
                // FU-7: Lightweight "bottom-of-chain" model — no alternatives exist
                // Auto-apply hostage core penalty φ=1, skip negotiation handshake
                penalty_phi = 1;
            }
        }

        // --- Tier 3: VIRTUAL FINISH TIME (VFT) ADMISSION CONTROL ---
        long v_queue = scheduler->getTotalWorkVolume(profiles);
        long v_running = job_tracker->getResidualWorkVolume();
        long expected_inf = profiles.at(key).dynamic_inf_ms.load();

        long est_wait = (v_queue + v_running) / total_inf_cores;
        long est_tat = est_wait + expected_inf;

        // C-23: Memory safety check during admission
        SystemSnapshot snap = SystemMonitor::takeSnapshot();
        bool mem_ok = (snap.mem_used_gb < (snap.total_mem_gb * sys.mem_safety_threshold - sys.mem_reserve_gb));

        // Tier 3: VFT check with configurable margin
        if (mem_ok && requested_slo >= static_cast<long>(est_tat * sys.vft_safety_margin)) {
            send(c_sock, "ACCEPTED", 8, 0);
            Job j;
            j.type = 'r'; j.client_sock = c_sock; j.model = final_model;
            j.batch = final_batch; j.arrival_ts = nowMs();
            j.requested_slo_ms = requested_slo;

            // Q-1: Store Cap_c fields
            j.device_class = device_class;
            j.cpu_freq = cpu_freq;
            j.ram_mb = ram_mb;
            j.net_type = net_type;
            j.is_default_cap = is_default_cap;

            // Q-2/Q-3: Store TTFR and negotiation results
            j.theta_est = theta_est;
            j.penalty_phi = penalty_phi;
            j.original_model = original_model_key;

            // Q-5: Generate unique request ID
            j.request_id = key + "_" + std::to_string(g_req_counter.fetch_add(1));

            scheduler->push(j);
        } else {
            // C-24/Q-4 Tier 3: Return SUGGESTED_SLO for negotiation
            std::string msg = "REJECTED_404:SUGGESTED_SLO:" + std::to_string(static_cast<long>(est_tat * sys.vft_safety_margin));
            send(c_sock, msg.c_str(), msg.length(), 0);
            close(c_sock);
        }
    }
}
/**
 * DISPATCHER: Strict Core Allocation with HoL Bypass (Q-9).
 */
 void SwiftServer::dispatcherLoop() {
    SystemSnapshot snap = SystemMonitor::takeSnapshot();
    // C-23: Use configurable memory safety threshold
    const double ram_limit = snap.total_mem_gb * sys.mem_safety_threshold - sys.mem_reserve_gb;

    // C-25: Track jobs since last checkpoint
    int jobs_since_checkpoint = 0;
    long last_checkpoint_ts = nowMs();

    while (running) {
        // FU-4: Centralized Triple Check — pass ResourceManager for core/memory gap checks
        auto job_opt = scheduler->popReadyJob(*fm, profiles, ram_limit,
                                               rm.get(), sys.mem_safety_threshold, sys.mem_reserve_gb);
        if (!job_opt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        Job j = *job_opt;
        std::string key = j.model + "_" + std::to_string(j.batch);

        // Q-9/Q-6: Determine target core count — scheduler-assigned or profile default
        int target_cores = (j.scheduled_cores > 0) ? j.scheduled_cores : profiles.at(key).threads;

        // Q-9: Strict Core Allocation — require exact core count
        auto cores = rm->acquireCoresStrict(target_cores);
        if (cores.empty()) {
            // Bypass: return job to queue and try next
            scheduler->push(j);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        j.assigned_cores = cores;
        j.assigned_port = rm->acquirePort();
        j.start_ts = nowMs();
        j.t_start_on = j.start_ts; // Q-5: Record J_on start timestamp

        // Track for VFT
        job_tracker->add(j.assigned_file, profiles.at(key).dynamic_inf_ms.load(), j.assigned_cores.size());

        // C-22: Dynamic watchdog timeout with θ_c (Q-10)
        // T_lease = (θ_c · e_on) · 1.2, capped at max_lease_timeout_s
        long exp_inf = profiles.at(key).dynamic_inf_ms.load();
        double theta_for_lease = std::max(1.0, j.theta_est);
        int timeout_s = std::min((int)((theta_for_lease * exp_inf * 1.2) / 1000.0 + 1), sys.max_lease_timeout_s);
        if (timeout_s < 10) timeout_s = 10;

        // Dispatch Worker Thread
        std::thread([this, j, key, timeout_s, &jobs_since_checkpoint, &last_checkpoint_ts]() mutable {
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

            // Q-2 TTFR Timer 2: External execution wrapper — measures actual J_on time
            auto start_exec = std::chrono::steady_clock::now();
            int rc = ProcessLauncher::runShellCommand(cmd, timeout_s);
            auto end_exec = std::chrono::steady_clock::now();
            long obs_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_exec - start_exec).count();

            // Q-2: Compute θ_act from Timer 2 (actual execution vs baseline)
            long baseline_inf = prof.inference_ms;
            if (baseline_inf > 0) {
                j.theta_act = std::max(1.0, (double)obs_ms / (double)baseline_inf);
            } else {
                j.theta_act = 1.0;
            }

            // C-05: Differentiated EWMA + C-26 + C-32/FU-5: Dual-phase Welford
            if (rc == 0) {
                long cur = prof.dynamic_inf_ms.load();
                prof.dynamic_inf_ms.store((long)((sys.ewma_alpha_on * obs_ms) + ((1.0 - sys.ewma_alpha_on) * cur)));
                prof.welfordUpdateOn((double)obs_ms);  // FU-5: Inference phase tracker
            } else if (rc != -1) {
                // C-26: Differentiated update for timeout (not crash)
                long cur = prof.dynamic_inf_ms.load();
                prof.dynamic_inf_ms.store((long)((sys.ewma_alpha_on * obs_ms) + ((1.0 - sys.ewma_alpha_on) * cur)));
            }
            // C-26: Ignore crashes (rc == -1) — do not update EWMA

            // Bookkeeping
            j.finish_ts = nowMs();
            long actual_tat = j.finish_ts - j.arrival_ts;

            // Q-11: Real-time Ψ metric tracking
            psi_total_jobs.fetch_add(1);
            psi_total_tat_ms.fetch_add(actual_tat);
            if (rc == 0 && actual_tat <= j.requested_slo_ms) {
                psi_success_count.fetch_add(1);
            }
            throughput_total_completed.fetch_add(1);

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

            // C-17: Notify SA scheduler of job completion for re-optimization
            SAScheduler* sa_sched = dynamic_cast<SAScheduler*>(scheduler.get());
            if (sa_sched) {
                sa_sched->notifyJobComplete();
            }

            // C-25: Asynchronous checkpointing
            jobs_since_checkpoint++;
            long now_ts = nowMs();
            if (jobs_since_checkpoint >= sys.profile_checkpoint_jobs ||
                (now_ts - last_checkpoint_ts) >= (sys.profile_checkpoint_secs * 1000L)) {
                saveDynamicProfile();
                jobs_since_checkpoint = 0;
                last_checkpoint_ts = now_ts;
            }

        }).detach();
    }
}

void SwiftServer::replenisherLoop() {
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
                    pj.arrival_ts = pj.start_ts; // OBSERVATION-02: Set Arrival_TS=Start_TS for internal preprocessing jobs
                    pj.t_start_pre = pj.start_ts; // Q-5

                    int rc = ProcessLauncher::runShellCommand(cmd, 600);

                    pj.t_fin_pre = nowMs(); // Q-5
                    pj.finish_ts = pj.t_fin_pre;
                    logger->logJob(pj, rc, SystemMonitor::takeSnapshot());

                    // FU-5: Dual-phase EWMA + Welford for preprocessing
                    if (rc == 0) {
                        long obs_pre_ms = pj.t_fin_pre - pj.t_start_pre;
                        auto& prof = profiles.at(key);
                        long cur_pre = prof.dynamic_pre_ms.load();
                        prof.dynamic_pre_ms.store((long)((sys.ewma_alpha_pre * obs_pre_ms) + ((1.0 - sys.ewma_alpha_pre) * cur_pre)));
                        prof.welfordUpdatePre((double)obs_pre_ms);
                    }

                    fm->setReady(key, prefix);
                    rm->releaseDealerSlot();
                    if (core != -1) rm->releaseCores({core});
                }).detach();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

/**
 * Q-8: Telemetry thread — polls /proc/meminfo at configurable interval.
 * Writes to mem_trace.csv: Timestamp_ms;Mem_Active_GB;Mem_Buffer_GB;Cores_In_Use;Total_System_Load
 */
void SwiftServer::telemetryLoop() {
    std::ofstream trace(sys.mem_trace_file, std::ios::app);
    if (trace.is_open() && trace.tellp() == 0) {
        trace << "Timestamp_ms;Mem_Active_GB;Mem_Buffer_GB;Cores_In_Use;Total_System_Load\n";
    }

    int interval = sys.telemetry_sample_rate_ms;
    if (interval < 50) interval = 50;
    if (interval > 1000) interval = 1000;

    while (running) {
        SystemSnapshot s = SystemMonitor::takeSnapshot();
        double mem_buffer = s.total_mem_gb - s.mem_used_gb;
        if (mem_buffer < 0) mem_buffer = 0;

        if (trace.is_open()) {
            trace << nowMs() << ";"
                  << s.mem_used_gb << ";"
                  << mem_buffer << ";"
                  << s.cpu_load << ";"  // Approximation for cores_in_use
                  << s.cpu_load << "\n";
            trace.flush();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }
}

void SwiftServer::updateDynamicMetrics(const std::string& key, long observed_inf, [[maybe_unused]] long observed_pre) {
    if (profiles.find(key) == profiles.end()) return;
    auto& prof = profiles.at(key);
    if (observed_inf > 0) {
        long cur = prof.dynamic_inf_ms.load();
        prof.dynamic_inf_ms.store((long)((sys.ewma_alpha_on * observed_inf) + ((1.0 - sys.ewma_alpha_on) * cur)));
        prof.welfordUpdate((double)observed_inf);
    }
}

void SwiftServer::saveDynamicProfile() {
    std::lock_guard<std::mutex> lock(mtx_profiles);
    std::string path = sys.dynamic_profile_path.empty() ? "logs/dynamic_profile.cfg" : sys.dynamic_profile_path;
    std::ofstream out(path);
    if (!out.is_open()) return;
    for (auto const& [key, p] : profiles) {
        // FU-5: Extended checkpoint with dual-phase Welford (σ_pre, σ_on)
        out << p.model << ", " << p.batch << ", " << p.preproc_ms << ", " << p.inference_ms << ", "
            << p.threads << ", " << p.max_buffer << ", " << p.file_size_mb << ", "
            << p.pre_mem_mb << ", " << p.inf_mem_mb << ", " << p.dynamic_inf_ms.load()
            << ", " << p.welford_on_mean << ", " << p.welfordStdDevOn()
            << ", " << p.dynamic_pre_ms.load()
            << ", " << p.welford_pre_mean << ", " << p.welfordStdDevPre() << "\n";
    }
}
