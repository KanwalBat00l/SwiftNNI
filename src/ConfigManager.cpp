#include "ConfigManager.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>

std::string ConfigManager::trim(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, (last - first + 1));
}

std::vector<std::string> ConfigManager::split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(trim(token));
    }
    return tokens;
}

SystemConfig ConfigManager::loadSystemConfig(const std::string& filename) {
    SystemConfig cfg;
    std::ifstream file(filename);
    if (!file.is_open()) throw std::runtime_error("Could not open config file: " + filename);

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos) continue;

        std::string key = trim(line.substr(0, sep));
        std::string val = trim(line.substr(sep + 1));

        if (key == "SERVER_IP") cfg.server_ip = val;
        else if (key == "SCHEDULER_PORT") cfg.scheduler_port = std::stoi(val);
        else if (key == "MAX_CONN") cfg.max_conn = std::stoi(val);
        else if (key == "TOTAL_CORES") cfg.total_cores = std::stoi(val);
        else if (key == "MAX_PREPROC_CONCURRENCY") cfg.max_preproc_concurrency = std::stoi(val);
        else if (key == "SCHEDULER_MODE") cfg.scheduler_mode = val;
        else if (key == "AGING_FACTOR") cfg.aging_factor = std::stod(val);
        else if (key == "BASE_PORT") cfg.base_port = std::stoi(val);
        else if (key == "PORT_RANGE") cfg.port_range = std::stoi(val);
        else if (key == "PRE_BASE_PORT") cfg.pre_base_port = std::stoi(val);
        else if (key == "TIMEOUT_FACTOR_PRE") cfg.timeout_factor_pre = std::stod(val);
        else if (key == "TIMEOUT_FACTOR_INF") cfg.timeout_factor_inf = std::stod(val);
        else if (key == "MIN_TIMEOUT_S") cfg.min_timeout_s = std::stoi(val);
        else if (key == "SERVER_CMD_TEMPLATE") cfg.server_cmd_template = val;
        else if (key == "PREPROC_CMD_TEMPLATE") cfg.preproc_cmd_template = val;
        else if (key == "SNNI_DIR") cfg.snni_dir = val;
        else if (key == "PROFILE_FILE") cfg.profile_file = val;
        else if (key == "LOG_FILE") cfg.log_file = val;
    }
    return cfg;
}

std::map<std::string, ModelProfile> ConfigManager::loadProfiles(const std::string& filename) {
    std::map<std::string, ModelProfile> profiles;
    std::ifstream file(filename);
    if (!file.is_open()) throw std::runtime_error("Could not open profile file: " + filename);

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        auto parts = split(line, ',');
        if (parts.size() < 5) continue;

        ModelProfile p;
        p.model = parts[0];
        p.batch = std::stoi(parts[1]);
        p.pre_ms = std::stol(parts[2]);
        p.inf_ms = std::stol(parts[3]);
        p.threads = std::stoi(parts[4]);
        
        // Generate the unique key: e.g., "simc2_4"
        p.key = p.model + "_" + std::to_string(p.batch);
        
        profiles[p.key] = p;
    }
    return profiles;
}