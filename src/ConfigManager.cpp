#include "ConfigManager.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

/**
 * Robust string sanitizer:
 * 1. Removes all non-printable/control characters like \r, \n, \t.
 * 2. Trims leading and trailing whitespace.
 */
std::string ConfigManager::trim(const std::string& s) {
    if (s.empty()) return "";
    std::string out;
    for (unsigned char c : s) {
        if (c >= 32 && c <= 126) out += c; 
    }
    size_t first = out.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    size_t last = out.find_last_not_of(" \t");
    return out.substr(first, (last - first + 1));
}

SystemConfig ConfigManager::loadSystemConfig(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) throw std::runtime_error("Fatal: Cannot open config " + filename);

    SystemConfig cfg;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream is_line(line);
        std::string key, value;
        if (std::getline(is_line, key, '=') && std::getline(is_line, value)) {
            key = trim(key); 
            value = trim(value);
            
            if (key == "SERVER_IP") cfg.server_ip = value;
            else if (key == "SCHEDULER_PORT") cfg.scheduler_port = std::stoi(value);
            else if (key == "MAX_CONN") cfg.max_conn = std::stoi(value);
            else if (key == "TOTAL_CORES") cfg.total_cores = std::stoi(value);
            else if (key == "SYSTEM_RESERVED_CORES") cfg.system_reserved_cores = std::stoi(value);
            else if (key == "MAX_PREPROC_CONCURRENCY") cfg.max_preproc_concurrency = std::stoi(value);
            else if (key == "ENABLE_CORE_PINNING") cfg.enable_core_pinning = (value == "true");
            else if (key == "BASE_PORT") cfg.base_port = std::stoi(value);
            else if (key == "PORT_RANGE") cfg.port_range = std::stoi(value);
            else if (key == "SCHEDULER_MODE") cfg.scheduler_mode = value;
            else if (key == "DEFAULT_SLO_K_FACTOR") cfg.default_slo_k_factor = std::stod(value);
            else if (key == "VFT_SAFETY_MARGIN") cfg.vft_safety_margin = std::stod(value);
            else if (key == "AGING_FACTOR") cfg.aging_factor = std::stod(value);
            else if (key == "SERVER_CMD_TEMPLATE") cfg.server_cmd_template = value;
            else if (key == "PREPROC_CMD_TEMPLATE") cfg.preproc_cmd_template = value;
            else if (key == "SNNI_DIR") cfg.snni_dir = value;
            else if (key == "LOG_FILE") cfg.log_file = value;
            else if (key == "SYS_FILE") cfg.sys_file = value;
            // New Config Keys for AI and Dynamic Profiling
            else if (key == "DQN_WEIGHTS_PATH") cfg.dqn_weights_path = value;
            else if (key == "DYNAMIC_PROFILE_FILE") cfg.dynamic_profile_path = value;
        }
    }
    return cfg;
}

std::map<std::string, ModelProfile> ConfigManager::loadProfiles(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) throw std::runtime_error("Fatal: Cannot open profile " + filename);

    std::map<std::string, ModelProfile> profiles;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> row;
        while (std::getline(ss, cell, ',')) row.push_back(trim(cell));

        if (row.size() >= 9) {
            ModelProfile p;
            p.model = row[0];
            p.batch = std::stoi(row[1]);
            p.preproc_ms = std::stol(row[2]);
            p.inference_ms = std::stol(row[3]);
            
            // Initializing Moving Averages with static baseline values
            p.dynamic_inf_ms.store(p.inference_ms);
            p.dynamic_pre_ms.store(p.preproc_ms);

            p.threads = std::stoi(row[4]);
            p.max_buffer = std::stoi(row[5]);
            p.file_size_mb = std::stoi(row[6]);
            p.pre_mem_mb = std::stoi(row[7]);
            p.inf_mem_mb = std::stoi(row[8]);
            
            p.key = p.model + "_" + std::to_string(p.batch);
            profiles[p.key] = p; // Uses our custom operator= defined in Types.hpp
        }
    }
    return profiles;
}