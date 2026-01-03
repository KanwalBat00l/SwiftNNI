#pragma once
#include <fstream>
#include <string>
#include <sstream>
#include <iostream>
#include "Types.hpp"

class SystemMonitor {
public:
    static SystemSnapshot takeSnapshot() {
        SystemSnapshot snap;
        snap.cpu_load = getCpuLoad();
        snap.mem_used_gb = getMemUsedGB();
        snap.energy_uj = getEnergyUJ();
        return snap;
    }

private:
    static double getCpuLoad() {
        std::ifstream file("/proc/loadavg");
        double load = 0.0;
        if (file >> load) return load;
        return 0.0;
    }

    static double getMemUsedGB() {
        std::ifstream file("/proc/meminfo");
        std::string line, label;
        long total = 0, avail = 0, value;
        while (std::getline(file, line)) {
            std::stringstream ss(line);
            ss >> label >> value;
            if (label == "MemTotal:") total = value;
            else if (label == "MemAvailable:") avail = value;
            if (total > 0 && avail > 0) break;
        }
        // Values in meminfo are KB. Convert to GB.
        return (double)(total - avail) / (1024.0 * 1024.0);
    }

    static long getEnergyUJ() {
        // Standard path for Intel/AMD RAPL (Package 0)
        std::ifstream file("/sys/class/powercap/intel-rapl:0/energy_uj");
        long energy = 0;
        if (file >> energy) return energy;
        return 0; // Return 0 if permission denied or path doesn't exist
    }
};