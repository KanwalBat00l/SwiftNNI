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
            // SA Scheduler Parameters
            else if (key == "SA_INITIAL_TEMP") cfg.sa_initial_temp = std::stod(value);
            else if (key == "SA_COOLING_RATE") cfg.sa_cooling_rate = std::stod(value);
            else if (key == "SA_MAX_ITERATIONS") cfg.sa_max_iterations = std::stoi(value);
            else if (key == "SA_WINDOW_SIZE") cfg.sa_window_size = std::stoi(value);
            // Differentiated EWMA (C-05)
            else if (key == "EWMA_ALPHA_PRE") cfg.ewma_alpha_pre = std::stod(value);
            else if (key == "EWMA_ALPHA_ON") cfg.ewma_alpha_on = std::stod(value);
            // Memory Safety (C-23)
            else if (key == "MEM_SAFETY_THRESHOLD") cfg.mem_safety_threshold = std::stod(value);
            else if (key == "MEM_RESERVE_GB") cfg.mem_reserve_gb = std::stod(value);
            // Config-driven normalization (C-11)
            else if (key == "TOTAL_MEMORY_MB") cfg.total_memory_mb = std::stoi(value);
            // Dynamic watchdog (C-22)
            else if (key == "MAX_LEASE_TIMEOUT_S") cfg.max_lease_timeout_s = std::stoi(value);
            // SA event trigger interval (C-17)
            else if (key == "SA_MIN_RETRIGGER_MS") cfg.sa_min_retrigger_ms = std::stoi(value);
            // Profile checkpoint (C-25)
            else if (key == "PROFILE_CHECKPOINT_JOBS") cfg.profile_checkpoint_jobs = std::stoi(value);
            else if (key == "PROFILE_CHECKPOINT_SECS") cfg.profile_checkpoint_secs = std::stoi(value);
            // Pre-processing port range (C-28)
            else if (key == "PRE_BASE_PORT") cfg.pre_base_port = std::stoi(value);
            // Q-8: Time-series memory telemetry
            else if (key == "TELEMETRY_SAMPLE_RATE_MS") cfg.telemetry_sample_rate_ms = std::stoi(value);
            else if (key == "ENABLE_TIME_SERIES_LOGGING") cfg.enable_time_series_logging = (value == "true");
            else if (key == "MEM_TRACE_FILE") cfg.mem_trace_file = value;
            // Q-3: Negotiation parameters
            else if (key == "THETA_NEGOTIATION_THRESHOLD") cfg.theta_negotiation_threshold = std::stod(value);
            else if (key == "NEGOTIATION_TIMEOUT_MS") cfg.negotiation_timeout_ms = std::stoi(value);
            // Q-2/FU-9: TTFR baseline RTT (legacy single-value and per-network-type)
            else if (key == "TTFR_BASELINE_RTT_MS") cfg.ttfr_baseline_rtt_ms = std::stod(value);
            else if (key == "TTFR_BASELINE_LAN_MS")  cfg.ttfr_baseline_lan_ms = std::stod(value);
            else if (key == "TTFR_BASELINE_WIFI_MS") cfg.ttfr_baseline_wifi_ms = std::stod(value);
            else if (key == "TTFR_BASELINE_5G_MS")   cfg.ttfr_baseline_5g_ms = std::stod(value);
            else if (key == "TTFR_BASELINE_WAN_MS")  cfg.ttfr_baseline_wan_ms = std::stod(value);
            // Tier 2: Static SLO factor
            else if (key == "SLO_FACTOR_TIER2") cfg.slo_factor_tier2 = std::stod(value);
            // FU-15: Linear Power Model fallback constants
            else if (key == "ENERGY_IDLE_POWER_W") cfg.energy_idle_power_w = std::stod(value);
            else if (key == "ENERGY_DYNAMIC_POWER_PER_CORE_W") cfg.energy_dynamic_power_per_core_w = std::stod(value);
            // Q-3/C-04/FU-6: Family-based n_alt model mapping
            // Format: "model_family:alt1_family,alt2_family;model2_family:alt3_family"
            // FU-6: Batch size is preserved from original request at negotiation time
            else if (key == "N_ALT_MAP") {
                // Parse semicolon-separated entries: "alexnet:simc2,hinet;vgg16:hinet,resnet50"
                std::istringstream entries(value);
                std::string entry;
                while (std::getline(entries, entry, ';')) {
                    entry = trim(entry);
                    if (entry.empty()) continue;
                    auto colon = entry.find(':');
                    if (colon == std::string::npos) continue;
                    std::string model_key = trim(entry.substr(0, colon));
                    std::string alts_str = trim(entry.substr(colon + 1));
                    std::vector<std::string> alts;
                    std::istringstream alt_ss(alts_str);
                    std::string alt;
                    while (std::getline(alt_ss, alt, ',')) {
                        alt = trim(alt);
                        if (!alt.empty()) alts.push_back(alt);
                    }
                    if (!alts.empty()) cfg.n_alt_map[model_key] = alts;
                }
            }
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

            // FU-14: Optional 10th column — per-model Amdahl's Law exponent k_n
            if (row.size() >= 10) {
                p.parallel_efficiency_k = std::stof(row[9]);
            }

            p.key = p.model + "_" + std::to_string(p.batch);
            profiles[p.key] = p; // Uses our custom operator= defined in Types.hpp
        }
    }
    return profiles;
}