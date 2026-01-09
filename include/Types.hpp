#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <atomic> // Must include this

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

    ModelProfile() : dynamic_inf_ms(0), dynamic_pre_ms(0) {}
    ModelProfile(const ModelProfile& other) {
        model = other.model; batch = other.batch;
        preproc_ms = other.preproc_ms; inference_ms = other.inference_ms;
        dynamic_inf_ms.store(other.dynamic_inf_ms.load());
        dynamic_pre_ms.store(other.dynamic_pre_ms.load());
        threads = other.threads; max_buffer = other.max_buffer;
        file_size_mb = other.file_size_mb; pre_mem_mb = other.pre_mem_mb;
        inf_mem_mb = other.inf_mem_mb; key = other.key;
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
        }
        return *this;
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
};



