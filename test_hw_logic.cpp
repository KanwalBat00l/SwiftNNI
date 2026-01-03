#include <iostream>
#include <vector>
#include <sched.h>
#include <unistd.h>
#include <sys/wait.h>
#include "SystemMonitor.hpp"

// Detect IDs assigned by Slurm
std::vector<int> getPhysCores() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
    std::vector<int> cores;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) cores.push_back(i);
    }
    return cores;
}

int main() {
    std::cout << "--- SAPPIS Hardware Logic Test ---" << std::endl;

    // 1. Check Physical Affinity
    auto cores = getPhysCores();
    std::cout << "[1] Cores assigned by Slurm: ";
    for(int id : cores) std::cout << id << " ";
    std::cout << "\nTotal: " << cores.size() << std::endl;

    if(cores.empty()) { std::cerr << "No cores detected!\n"; return 1; }

    // 2. Test taskset Pinning
    // Pick the last core ID in our allocation
    int target_core = cores.back();
    std::cout << "[2] Testing taskset on Core " << target_core << "..." << std::endl;
    
    std::string cmd = "taskset -c " + std::to_string(target_core) + " sleep 1";
    int rc = system(cmd.c_str());
    if (rc == 0) std::cout << "SUCCESS: taskset executed on physical ID " << target_core << std::endl;
    else std::cout << "FAILED: taskset rejected. Check core permissions." << std::endl;

    // 3. Test Telemetry
    std::cout << "[3] Testing Telemetry Collection..." << std::endl;
    SystemSnapshot snap = SystemMonitor::takeSnapshot();
    std::cout << "CPU Load (1m): " << snap.cpu_load << std::endl;
    std::cout << "Mem Used (GB): " << snap.mem_used_gb << std::endl;
    std::cout << "Energy (uJ):   " << snap.energy_uj << " (Note: 0 if no RAPL access)" << std::endl;

    return 0;
}