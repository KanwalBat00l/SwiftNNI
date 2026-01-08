#include "SystemMonitor.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <unistd.h> // REQUIRED for sysconf and memory constants

/**
 * @brief Captures a full programmatic snapshot of the node's performance and capacity.
 * 
 * Logic:
 * 1. Reads /proc/loadavg for current CPU pressure.
 * 2. Reads /proc/meminfo to detect TOTAL physical RAM and currently available RAM.
 * 3. Executes Hybrid Energy Audit: Attempts hardware RAPL reading; if blocked/zero, 
 *    falls back to Slurm 'sacct' accounting for the current Job ID.
 */
 SystemSnapshot SystemMonitor::takeSnapshot() {
    SystemSnapshot snap;
    
    // 1. CPU Load Detection
    snap.cpu_load = getCpuLoad();

    // 2. RAM Detection (Capacity and Usage)
    // We parse /proc/meminfo to avoid hardcoding 32GB/64GB.
    std::ifstream mem_file("/proc/meminfo");
    std::string line, label;
    long total_kb = 0, avail_kb = 0, value;
    
    while (std::getline(mem_file, line)) {
        std::stringstream ss(line);
        ss >> label >> value;
        if (label == "MemTotal:") total_kb = value;
        else if (label == "MemAvailable:") avail_kb = value;
    }
    
    // Convert KB to GB for our Types.hpp structure
    snap.total_mem_gb = static_cast<double>(total_kb) / (1024.0 * 1024.0);
    snap.mem_used_gb = static_cast<double>(total_kb - avail_kb) / (1024.0 * 1024.0);

    // 3. Hybrid Energy Audit (Preserved from original logic)
    long energy = getEnergyFromRAPL();
    if (energy == 0) {
        energy = getEnergyFromSlurm();
    }
    snap.energy_uj = energy;
    
    return snap;
}

double SystemMonitor::getCpuLoad() {
    std::ifstream file("/proc/loadavg");
    double load = 0.0;
    if (file >> load) return load;
    return 0.0;
}

long SystemMonitor::getEnergyFromRAPL() {
    std::ifstream file("/sys/class/powercap/intel-rapl:0/energy_uj");
    long energy = 0;
    if (file >> energy) return energy;
    return 0;
}

long SystemMonitor::getEnergyFromSlurm() {
    char* job_id = getenv("SLURM_JOB_ID");
    if (!job_id) return 0;

    std::string cmd = "sacct -j " + std::string(job_id) + " -n -b --format=ConsumedEnergy 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return 0;

    char buffer[128];
    std::string result = "";
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) result = buffer;
    pclose(pipe);

    // Trim whitespace
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);

    return parseEnergyString(result);
}

long SystemMonitor::parseEnergyString(std::string s) {
    if (s.empty() || s == "0") return 0;
    double value = 0.0;
    char unit = 'J';

    char last = s.back();
    if (last == 'J') {
        s.pop_back();
        if (!s.empty()) last = s.back(); 
    }

    if (isdigit(last)) {
        value = std::stod(s);
        unit = 'J';
    } else {
        unit = last;
        s.pop_back();
        value = std::stod(s);
    }

    double joules = 0;
    switch (unit) {
        case 'K': joules = value * 1000.0; break;
        case 'M': joules = value * 1000000.0; break;
        case 'G': joules = value * 1000000000.0; break;
        default:  joules = value; break;
    }
    return static_cast<long>(joules * 1000000.0); // Convert to microjoules
}

/**
 * @brief Programmatically detects the total physical RAM available on the node.
 * Returns value in GB.
 */
double SystemMonitor::getTotalMemGB() {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    // bytes -> KB -> MB -> GB
    return static_cast<double>(pages * page_size) / (1024.0 * 1024.0 * 1024.0);
}

double SystemMonitor::getMemUsedGB() {
    std::ifstream file("/proc/meminfo");
    std::string line, label;
    long total = 0, avail = 0, value;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        ss >> label >> value;
        if (label == "MemTotal:") total = value;
        else if (label == "MemAvailable:") avail = value;
    }
    return (double)(total - avail) / (1024.0 * 1024.0);
}


