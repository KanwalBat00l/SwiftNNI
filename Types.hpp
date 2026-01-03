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
    int scheduler_port;
    std::string server_ip;
    int max_conn;
    std::string scheduler_mode;
    std::string snni_dir;
    std::string log_file;
    std::string sys_file;
    int total_cores;
    int base_port;
    int port_range;
    int pre_base_port;
    double aging_factor;
    int max_preproc_concurrency;
    double default_slo_k_factor;

    std::string server_cmd_template;
    std::string client_cmd_template;
    std::string preproc_cmd_template;
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