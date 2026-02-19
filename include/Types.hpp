#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <atomic> // Must include this
#include <cmath>
#include <sstream>

/**
 * @enum FileStatus
 * @brief Represents the lifecycle of a SHARK pre-processed data file (.dat).
 */
enum class FileStatus { 
    CREATING,  /**< Dealer is currently generating the file. */
    READY,     /**< File is available in the proactive buffer. */
    IN_USE,    /**< File is currently being used by an active inference. */
    USED       /**< Inference complete; file is marked for deletion. */
};

/**
 * @struct ModelProfile
 * @brief Dynamic metadata for a model. 
 * Note: Custom copy/assignment logic is required because std::atomic is not copyable.
 */
 struct ModelProfile {
    std::string model;
    int batch;
    long preproc_ms;
    long inference_ms;
    std::atomic<long> dynamic_inf_ms;
    std::atomic<long> dynamic_pre_ms;
    int threads;
    int max_buffer;
    int file_size_mb;
    int pre_mem_mb;
    int inf_mem_mb;
    std::string key;

    // FU-14: Per-model Amdahl's Law parallel efficiency exponent k_n
    // T(p) = e_pre + (theta_c * e_on) / p^k_n  (replaces global k=0.8)
    float parallel_efficiency_k = 0.8f;

    // FU-5: Dual-phase Welford's online variance tracking (C-32)
    // Separate trackers for preprocessing (σ_pre) and inference (σ_on)
    long welford_pre_count = 0;
    double welford_pre_mean = 0.0;
    double welford_pre_m2 = 0.0;

    long welford_on_count = 0;
    double welford_on_mean = 0.0;
    double welford_on_m2 = 0.0;

    ModelProfile() : dynamic_inf_ms(0), dynamic_pre_ms(0) {}
    ModelProfile(const ModelProfile& other) {
        model = other.model; batch = other.batch;
        preproc_ms = other.preproc_ms; inference_ms = other.inference_ms;
        dynamic_inf_ms.store(other.dynamic_inf_ms.load());
        dynamic_pre_ms.store(other.dynamic_pre_ms.load());
        threads = other.threads; max_buffer = other.max_buffer;
        file_size_mb = other.file_size_mb; pre_mem_mb = other.pre_mem_mb;
        inf_mem_mb = other.inf_mem_mb; key = other.key;
        // FU-14: Per-model Amdahl exponent
        parallel_efficiency_k = other.parallel_efficiency_k;
        // FU-5: Dual-phase Welford copy
        welford_pre_count = other.welford_pre_count;
        welford_pre_mean = other.welford_pre_mean;
        welford_pre_m2 = other.welford_pre_m2;
        welford_on_count = other.welford_on_count;
        welford_on_mean = other.welford_on_mean;
        welford_on_m2 = other.welford_on_m2;
    }
    ModelProfile& operator=(const ModelProfile& other) {
        if (this != &other) {
            model = other.model; batch = other.batch;
            preproc_ms = other.preproc_ms; inference_ms = other.inference_ms;
            dynamic_inf_ms.store(other.dynamic_inf_ms.load());
            dynamic_pre_ms.store(other.dynamic_pre_ms.load());
            threads = other.threads; max_buffer = other.max_buffer;
            file_size_mb = other.file_size_mb; pre_mem_mb = other.pre_mem_mb;
            inf_mem_mb = other.inf_mem_mb; key = other.key;
            // FU-14: Per-model Amdahl exponent
            parallel_efficiency_k = other.parallel_efficiency_k;
            // FU-5: Dual-phase Welford copy
            welford_pre_count = other.welford_pre_count;
            welford_pre_mean = other.welford_pre_mean;
            welford_pre_m2 = other.welford_pre_m2;
            welford_on_count = other.welford_on_count;
            welford_on_mean = other.welford_on_mean;
            welford_on_m2 = other.welford_on_m2;
        }
        return *this;
    }

    // FU-5: Dual-phase Welford's Algorithm (C-32)
    // Update inference (online) phase statistics
    void welfordUpdate(double observation) {
        welfordUpdateOn(observation);
    }
    void welfordUpdateOn(double observation) {
        welford_on_count++;
        double delta = observation - welford_on_mean;
        welford_on_mean += delta / (double)welford_on_count;
        double delta2 = observation - welford_on_mean;
        welford_on_m2 += delta * delta2;
    }
    // Update preprocessing phase statistics
    void welfordUpdatePre(double observation) {
        welford_pre_count++;
        double delta = observation - welford_pre_mean;
        welford_pre_mean += delta / (double)welford_pre_count;
        double delta2 = observation - welford_pre_mean;
        welford_pre_m2 += delta * delta2;
    }

    // Legacy: returns inference stddev (backward compatible)
    double welfordStdDev() const {
        return welfordStdDevOn();
    }
    double welfordStdDevOn() const {
        return (welford_on_count < 2) ? 0.0 : std::sqrt(welford_on_m2 / (double)(welford_on_count - 1));
    }
    double welfordStdDevPre() const {
        return (welford_pre_count < 2) ? 0.0 : std::sqrt(welford_pre_m2 / (double)(welford_pre_count - 1));
    }
};


/**
 * @struct SystemConfig
 * @brief Global server settings and hardware constraints.
 * 
 * Loaded from config.cfg. Controls the behavior of the scheduler and resource pools.
 */
 struct SystemConfig {
    std::string server_ip;
    int scheduler_port;
    int max_conn;
    int total_cores;
    int system_reserved_cores;
    int max_preproc_concurrency;
    bool enable_core_pinning;
    int base_port;
    int port_range;
    std::string scheduler_mode;
    double default_slo_k_factor;
    double aging_factor;
    double vft_safety_margin; // NEW: From config
    std::string server_cmd_template;
    std::string preproc_cmd_template;
    std::string snni_dir;
    std::string log_file;
    std::string sys_file;
    std::string dqn_weights_path;
    std::string dynamic_profile_path;

    // SA Scheduler Parameters
    double sa_initial_temp = 100.0;
    double sa_cooling_rate = 0.92;
    int sa_max_iterations = 80;
    int sa_window_size = 100;  // C-18: Full-queue up to depth 100

    // Differentiated EWMA parameters (C-05)
    double ewma_alpha_pre = 0.2;
    double ewma_alpha_on  = 0.4;

    // Memory safety threshold (C-23): fraction of total RAM
    double mem_safety_threshold = 0.90;
    double mem_reserve_gb = 2.0;  // Hard reserve in GB

    // Total memory for config-driven normalization (C-11)
    int total_memory_mb = 0;  // 0 = auto-detect from SystemMonitor

    // Dynamic watchdog cap (C-22)
    int max_lease_timeout_s = 1800;  // Global cap in seconds

    // SA event-trigger minimum interval (C-17)
    int sa_min_retrigger_ms = 50;

    // Profile checkpoint interval (C-25)
    int profile_checkpoint_jobs = 10;
    int profile_checkpoint_secs = 30;

    // Pre-processing port range (C-28)
    int pre_base_port = 46000;

    // Q-8: Time-series memory logging
    int telemetry_sample_rate_ms = 100;  // Default 100ms, range 50-1000
    bool enable_time_series_logging = true;
    std::string mem_trace_file = "logs/mem_trace.csv";

    // Q-3: Negotiation threshold
    double theta_negotiation_threshold = 1.5;
    int negotiation_timeout_ms = 200;  // Client response timeout

    // Q-2/FU-9: TTFR baseline RTT in milliseconds — per-network-type
    double ttfr_baseline_rtt_ms = 5.0;  // Default/fallback 5ms for LAN
    // FU-9: Differentiated baselines per net_type: 0=LAN, 1=WiFi, 2=5G, 3=WAN
    double ttfr_baseline_lan_ms  = 5.0;
    double ttfr_baseline_wifi_ms = 20.0;
    double ttfr_baseline_5g_ms   = 40.0;
    double ttfr_baseline_wan_ms  = 100.0;

    // Q-3/C-04: n_alt model mappings — model_key → list of lighter alternatives
    std::map<std::string, std::vector<std::string>> n_alt_map;

    // C-10: Default SLO factor for Tier 2 static filter
    double slo_factor_tier2 = 1.5;  // D must be >= slo_factor * E_{i,c}

    // FU-15: Linear Power Model fallback constants
    // Used when RAPL=0 and Slurm=0: Energy = (P_idle + p_J * P_dynamic_per_core) * TAT
    double energy_idle_power_w = 100.0;          // P_idle in watts
    double energy_dynamic_power_per_core_w = 10.0; // P_dynamic per active core in watts
};


/**
 * @struct SystemSnapshot
 * @brief Real-time telemetry data.
 * 
 * Used for Iteration 9 energy modeling and system-aware scheduling.
 */
 struct SystemSnapshot {
    double cpu_load;
    double mem_used_gb;
    double total_mem_gb; 
    long energy_uj;
};

/**
 * @struct Job
 * @brief The primary unit of work in SAPPIS.
 * 
 * Carries all state from initial client request through to final cleanup.
 */
 
struct Job {
    char type; // 'r' for request, 'p' for pre-processing
    int client_sock;
    std::string model;
    int batch;
    long arrival_ts;
    long requested_slo_ms;
    long start_ts = 0;
    long finish_ts = 0;
    std::string assigned_file;
    std::vector<int> assigned_cores;
    int assigned_port;

    // Q-1: Cap_c Capability Vector fields (C-02)
    int device_class = 4;     // 0=IoT,1=Mobile,2=Tablet,3=Laptop,4=Desktop
    float cpu_freq = 3.0f;    // Client CPU frequency in GHz
    int ram_mb = 8192;        // Client RAM in MB
    int net_type = 0;         // 0=LAN,1=WiFi,2=5G,3=WAN
    bool is_default_cap = true; // true if symmetric defaults were used

    // Q-2/Q-3: Slowness factor and negotiation
    double theta_est = 1.0;   // Estimated θ_c from TTFR Timer 1 (TCP RTT)
    double theta_act = 0.0;   // Ground-truth θ_c from TTFR Timer 2 (execution wrapper)
    int penalty_phi = 0;      // Priority Penalty: 1 if negotiation refused, else 0
    std::string original_model; // Original model before negotiation (empty if no negotiation)

    // Q-5: Request ID for 21-column CSV
    std::string request_id;
    long t_start_pre = 0;     // Timestamp when J_pre started
    long t_fin_pre = 0;       // Timestamp when J_pre finished
    long t_start_on = 0;      // Timestamp when J_on started

    // Q-6: Malleable core count determined by scheduler
    int scheduled_cores = 0;  // Core count chosen by SA/DQN (0 = use profile default)
};



