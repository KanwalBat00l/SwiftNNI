#include <iostream>
#include <vector>
#include <sched.h>
#include <unistd.h>
#include "Types.hpp"
#include "ConfigManager.hpp"
#include "SystemMonitor.hpp"

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
    std::cout << "--- SAPPIS Pre-Flight Hardware Audit ---" << std::endl;

    // 1. Slurm Core Affinity
    auto cores = getPhysCores();
    std::cout << "[1] Slurm Core IDs: ";
    for(int id : cores) std::cout << id << " ";
    std::cout << "\n    Total Cores Available: " << cores.size() << std::endl;

    // 2. taskset Check
    if(!cores.empty()) {
        int test_core = cores.back();
        std::string cmd = "taskset -c " + std::to_string(test_core) + " sleep 0.1 2>/dev/null";
        int rc = system(cmd.c_str());
        std::cout << "[2] Taskset Capability: " << (rc == 0 ? "SUCCESS" : "FAILED") << std::endl;
    }

    // 3. Energy Source Audit
    std::cout << "[3] Energy Monitoring Audit:" << std::endl;
    char* job_id = getenv("SLURM_JOB_ID");
    if (job_id) {
        std::cout << "    Detected Slurm Job ID: " << job_id << std::endl;
        // Mocking a sacct call
        std::string sacct_cmd = "sacct -j " + std::string(job_id) + " --format=ConsumedEnergy -n -b";
        std::cout << "    Testing sacct access... " << std::endl;
        system(sacct_cmd.c_str());
    } else {
        std::cout << "    WARNING: Not running inside a Slurm Job. sacct fallback disabled." << std::endl;
    }

    return 0;
}