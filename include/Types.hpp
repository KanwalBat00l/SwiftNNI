#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>

/**
 * @enum FileStatus
 * @brief Represents the lifecycle of a single-use SHARK pre-processed file (.vmfb).
 */
enum class FileStatus { 
    DIRTY,      /**< File does not exist or was just used; needs generation. */
    GENERATING, /**< Pre-processor (Mode 2) is currently creating the file. */
    READY       /**< File is available on disk for inference. */
    // Note: 'IN_USE' is handled by a separate counter to allow 
    // the "generate-next-while-current-runs" logic.
};

/**
 * @struct ModelProfile
 * @brief Static performance data loaded from profile.cfg.
 * Used for SJF scheduling and dynamic timeout calculation.
 */
struct ModelProfile {
    std::string model;
    int batch;
    long pre_ms;    /**< Static profiled pre-processing time. */
    long inf_ms;    /**< Static profiled inference time (SJF Weight). */
    int threads;    /**< Recommended threads for this specific batch. */
    std::string key; /**< "model_batch" lookup key. */
};

/**
 * @struct SystemConfig
 * @brief Global server settings loaded from config.cfg.
 */
struct SystemConfig {
    // Networking
    std::string server_ip;
    int scheduler_port;
    int max_conn;

    // Resources
    int total_cores;
    int max_preproc_concurrency;

    // Scheduling Logic
    std::string scheduler_mode; // "FCFS" or "SJF"
    double aging_factor;        // Penalty reduction per ms of waiting

    // Dynamic Watchdog Factors
    double timeout_factor_pre;
    double timeout_factor_inf;
    int min_timeout_s;

    // Port Management
    int base_port;
    int port_range;
    int pre_base_port;

    // Templates
    std::string server_cmd_template;
    std::string preproc_cmd_template;

    // Paths
    std::string snni_dir;
    std::string profile_file;
    std::string log_file;
};

/**
 * @struct Job
 * @brief The unit of work. Minimalist version for SwiftNNI.
 */
struct Job {
    // Request Meta
    char type;         /**< 'r' (Request) or 'a' (Advance Notice). */
    int client_sock;
    std::string model;
    int batch;
    
    // Timing
    long arrival_ts;   /**< Arrival in ms since epoch. */
    long start_ts = 0;
    long finish_ts = 0;
    
    // Scheduling parameters
    long est_inf_ms;   /**< From profile; used for SJF score. */
    long est_pre_ms;   /**< From profile; used for watchdog. */
    
    // Execution details
    int assigned_port;
    int assigned_threads;
    std::string assigned_file;
    std::string cmd;
    int exit_code = -1;
};

/**
 * @struct CmdResult
 * @brief Result of a process execution.
 */
struct CmdResult {
    int rc;
    std::string reason;
    bool timed_out;
};