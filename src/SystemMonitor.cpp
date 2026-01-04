#include "SystemMonitor.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <cstdio>
#include <cstdlib>

SystemSnapshot SystemMonitor::takeSnapshot() {
    SystemSnapshot snap;
    snap.cpu_load = getCpuLoad();
    snap.mem_used_gb = getMemUsedGB();

    // Hybrid Logic: Try RAPL first, if 0, try Slurm sacct
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

long SystemMonitor::getEnergyFromRAPL() {
    // Path for AMD/Intel RAPL Package 0
    std::ifstream file("/sys/class/powercap/intel-rapl:0/energy_uj");
    long energy = 0;
    if (file >> energy) return energy;
    return 0;
}

long SystemMonitor::getEnergyFromSlurm() {
    char* job_id = getenv("SLURM_JOB_ID");
    if (!job_id) return 0;

    // Command: sacct -j <ID> -n -b --format=ConsumedEnergy
    std::string cmd = "sacct -j " + std::string(job_id) + " -n -b --format=ConsumedEnergy 2>/dev/null";
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return 0;

    char buffer[128];
    std::string result = "";
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) result = buffer;
    pclose(pipe);

    // Clean the string (remove spaces/newlines)
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);

    return parseEnergyString(result);
}

long SystemMonitor::parseEnergyString(std::string s) {
    if (s.empty() || s == "0") return 0;

    double value = 0.0;
    char unit = 'J';

    // Check last character for units (K, M, G, J)
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

    // Convert to Joules
    double joules = 0;
    switch (unit) {
        case 'K': joules = value * 1000.0; break;
        case 'M': joules = value * 1000000.0; break;
        case 'G': joules = value * 1000000000.0; break;
        default:  joules = value; break;
    }

    // Convert Joules to Microjoules (uJ) for Types.hpp
    return (long)(joules * 1000000.0);
}