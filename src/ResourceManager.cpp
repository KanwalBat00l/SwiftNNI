#include "ResourceManager.hpp"
#include <sched.h>
#include <algorithm>
#include <sstream>
#include <iostream>

ResourceManager::ResourceManager([[maybe_unused]] int total_cores, int port_base, int port_range, int max_preproc, bool enable_pinning)
    : pinning_enabled(enable_pinning), max_preproc(max_preproc), active_preproc(0) {
    
    // 1. Detect physical core IDs assigned by Slurm
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
    
    std::vector<int> physical_ids;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) physical_ids.push_back(i);
    }

    // 2. Core Partitioning (Bucket Logic)
    // Rule: Skip first 2 cores (0, 1) for SAPPIS server overhead.
    size_t start_idx = 2; 
    
    // Fill the Dealer Pool (limited by MAX_PREPROC_CONCURRENCY)
    for (size_t i = 0; i < static_cast<size_t>(max_preproc) && (start_idx + i) < physical_ids.size(); ++i) {
        dealer_pool.push_back(physical_ids[start_idx + i]);
    }
    
    // Fill the Inference Pool with the remaining physical cores
    for (size_t i = start_idx + static_cast<size_t>(max_preproc); i < physical_ids.size(); ++i) {
        inference_pool.push_back(physical_ids[i]);
    }

    // 3. Initialize Network Ports
    for (int i = 0; i < port_range; ++i) {
        available_ports.insert(port_base + i);
    }
}

/**
 * FIXED: Implementation of acquireDealerCore
 */
int ResourceManager::acquireDealerCore() {
    std::lock_guard<std::mutex> lock(mtx);
    if (!pinning_enabled) return -1;

    for (int core : dealer_pool) {
        if (busy_cores.find(core) == busy_cores.end()) {
            busy_cores.insert(core);
            return core;
        }
    }
    return -1; // All dealer cores in bucket are busy
}

std::vector<int> ResourceManager::acquireInferenceCores(int count) {
    std::lock_guard<std::mutex> lock(mtx);
    size_t target_count = static_cast<size_t>(count);

    if (!pinning_enabled) return std::vector<int>(count, -1);

    std::vector<int> allocated;
    for (int core : inference_pool) {
        if (busy_cores.find(core) == busy_cores.end()) {
            allocated.push_back(core);
            if (allocated.size() == target_count) break;
        }
    }

    if (allocated.size() == target_count) {
        for (int c : allocated) busy_cores.insert(c);
        return allocated;
    }
    return {}; // Not enough cores available to satisfy request
}

void ResourceManager::releaseCores(const std::vector<int>& cores) {
    std::lock_guard<std::mutex> lock(mtx);
    for (int c : cores) {
        if (c != -1) busy_cores.erase(c);
    }
}

int ResourceManager::acquirePort() {
    std::lock_guard<std::mutex> lock(mtx);
    if (available_ports.empty()) return -1;
    int p = *available_ports.begin();
    available_ports.erase(available_ports.begin());
    return p;
}

void ResourceManager::releasePort(int port) {
    std::lock_guard<std::mutex> lock(mtx);
    available_ports.insert(port);
}

bool ResourceManager::hasCapacityForDealer() {
    std::lock_guard<std::mutex> lock(mtx);
    if (active_preproc < max_preproc) {
        active_preproc++;
        return true;
    }
    return false;
}

void ResourceManager::releaseDealerSlot() {
    std::lock_guard<std::mutex> lock(mtx);
    if (active_preproc > 0) active_preproc--;
}

std::string ResourceManager::coresToString(const std::vector<int>& cores) {
    if (!pinning_enabled || cores.empty() || cores[0] == -1) return "";
    
    std::stringstream ss;
    for (size_t i = 0; i < cores.size(); ++i) {
        ss << cores[i] << (i == cores.size() - 1 ? "" : ",");
    }
    return ss.str();
}