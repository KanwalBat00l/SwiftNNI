#pragma once
#include <string>
#include <vector>
#include <map>
#include <chrono>

enum class FileStatus { CREATING, READY, IN_USE, USED };

struct ModelProfile {
    std::string model;
    int batch;
    long preproc_ms;
    long inference_ms;
    int threads;
    int max_buffer;
    std::string key;
};

struct SystemConfig {
    std::string server_ip;
    int scheduler_port;
    int max_conn;
    int total_cores;
    int system_reserved_cores; // NEW
    int max_preproc_concurrency;
    
    int base_port;
    int port_range;
    int pre_base_port;
    
    std::string scheduler_mode;
    double default_slo_k_factor;
    double aging_factor;
    
    std::string server_cmd_template;
    std::string preproc_cmd_template;
    std::string snni_dir;
    std::string log_file;
};


struct SystemSnapshot {
    double cpu_load;
    double mem_used_gb;
    long energy_uj; 
};

struct Job {
    char type; 
    int client_sock;
    std::string model;
    int batch;
    long arrival_ts;
    long requested_slo_ms;
    
    long start_ts = 0;
    long finish_ts = 0;

    std::string assigned_file;
    int assigned_threads;
    int assigned_port;
};