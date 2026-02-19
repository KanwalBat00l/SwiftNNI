#include "ResourceManager.hpp"
#include <sched.h>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

/**
 * Helper: Physically probes the Linux Kernel to see if a port is in TIME_WAIT or truly free.
 */
static bool isPortFree(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    // Set SO_REUSEADDR to ensure we can bind even if previous usage is cleaning up
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    bool success = (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close(sock);
    return success;
}

ResourceManager::ResourceManager(int requested_cores, int port_base, int port_range, int max_preproc, bool enable_pinning)
    : pinning_enabled(enable_pinning), max_preproc(max_preproc), active_preproc(0) {
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
    
    std::vector<int> actual_available_ids;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) actual_available_ids.push_back(i);
    }

    size_t limit = std::min((size_t)requested_cores, actual_available_ids.size());
    size_t start_idx = 2; // Reserved for System/SAPPIS
    
    // Partition buckets
    for (size_t i = 0; i < static_cast<size_t>(max_preproc) && (start_idx + i) < limit; ++i) {
        dealer_pool.push_back(actual_available_ids[start_idx + i]);
    }
    for (size_t i = start_idx + static_cast<size_t>(max_preproc); i < limit; ++i) {
        inference_pool.push_back(actual_available_ids[i]);
    }

    // --- NEW: Create the "Inference Boundary" string ---
    std::stringstream ss;
    for (size_t i = 0; i < inference_pool.size(); ++i) {
        ss << inference_pool[i] << (i == inference_pool.size() - 1 ? "" : ",");
    }
    all_inference_cores_str = ss.str();

    for (int i = 0; i < port_range; ++i) {
        available_ports_vec.push_back(port_base + i);
    }
    next_port_idx = 0;
}

/**
 * FIXED: Inference-Pool Fallback.
 * Ensures the job never exits its bucket, even when under-allocated.
 */
std::string ResourceManager::coresToString(const std::vector<int>& cores) {
    // 1. If pinning is disabled globally, return empty (StringUtils will strip taskset)
    if (!pinning_enabled) return "";

    // 2. If the Dispatcher couldn't find free cores (Saturation case),
    // return the entire Inference Bucket string. The job will share cores with
    // other inferences but stay away from Dealers and System cores.
    if (cores.empty() || cores[0] == -1) {
        return all_inference_cores_str;
    }
    
    // 3. Normal case: return the specific assigned IDs
    std::stringstream ss;
    for (size_t i = 0; i < cores.size(); ++i) {
        ss << cores[i] << (i == cores.size() - 1 ? "" : ",");
    }
    return ss.str();
}

/**
 * FIXED: Implementation of acquirePort using Round-Robin entropy.
 */
int ResourceManager::acquirePort() {
    std::lock_guard<std::mutex> lock(mtx);
    size_t pool_size = available_ports_vec.size();

    // Iterate through the pool starting from the last used position
    for (size_t i = 0; i < pool_size; ++i) {
        size_t idx = (next_port_idx + i) % pool_size;
        int candidate = available_ports_vec[idx];

        // Check if SAPPIS thinks it's busy
        if (busy_ports.find(candidate) == busy_ports.end()) {
            // Check if the OS Kernel thinks it's busy (TIME_WAIT check)
            if (isPortFree(candidate)) {
                busy_ports.insert(candidate);
                next_port_idx = (idx + 1) % pool_size; // Move needle for next job
                return candidate;
            }
        }
    }
    return -1; // No ports physically free
}

void ResourceManager::releasePort(int port) {
    std::lock_guard<std::mutex> lock(mtx);
    busy_ports.erase(port);
}

int ResourceManager::acquireDealerCore() {
    std::lock_guard<std::mutex> lock(mtx);
    if (!pinning_enabled) return -1;
    for (int core : dealer_pool) {
        if (busy_cores.find(core) == busy_cores.end()) {
            busy_cores.insert(core);
            return core;
        }
    }
    return -1;
}

std::vector<int> ResourceManager::acquireInferenceCores(int count) {
    std::lock_guard<std::mutex> lock(mtx);
    size_t target = static_cast<size_t>(count);
    if (!pinning_enabled) return std::vector<int>(count, -1);

    std::vector<int> allocated;
    for (int core : inference_pool) {
        if (busy_cores.find(core) == busy_cores.end()) {
            allocated.push_back(core);
            if (allocated.size() == target) break;
        }
    }
    if (allocated.size() == target) {
        for (int c : allocated) busy_cores.insert(c);
        return allocated;
    }
    return {};
}

std::vector<int> ResourceManager::acquireCoresElastic(int requested) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!pinning_enabled) return std::vector<int>(requested, -1);

    std::vector<int> allocated;
    for (int core : inference_pool) {
        if (busy_cores.find(core) == busy_cores.end()) {
            allocated.push_back(core);
            busy_cores.insert(core);
            if (allocated.size() == static_cast<size_t>(requested)) break;
        }
    }
    return allocated;
}

/**
 * FU-4: Query-only core availability check — does NOT acquire.
 * Returns true if 'required' free inference cores exist.
 */
bool ResourceManager::hasFreeInferenceCores(int required) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!pinning_enabled) return true;  // Unpinned mode always "has" cores
    int free_count = 0;
    for (int core : inference_pool) {
        if (busy_cores.find(core) == busy_cores.end()) {
            free_count++;
            if (free_count >= required) return true;
        }
    }
    return false;
}

/**
 * Q-9: Strict Core Allocation — returns exact count or empty vector.
 * Unlike acquireCoresElastic, this returns {} if the exact number isn't available.
 */
std::vector<int> ResourceManager::acquireCoresStrict(int required) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!pinning_enabled) return std::vector<int>(required, -1);

    // First count free cores
    std::vector<int> candidates;
    for (int core : inference_pool) {
        if (busy_cores.find(core) == busy_cores.end()) {
            candidates.push_back(core);
            if (candidates.size() == static_cast<size_t>(required)) break;
        }
    }

    // Strict: must match exactly
    if (candidates.size() == static_cast<size_t>(required)) {
        for (int c : candidates) busy_cores.insert(c);
        return candidates;
    }
    return {}; // Bypass: insufficient cores
}

void ResourceManager::releaseCores(const std::vector<int>& cores) {
    std::lock_guard<std::mutex> lock(mtx);
    for (int c : cores) { if (c != -1) busy_cores.erase(c); }
}

bool ResourceManager::hasCapacityForDealer() {
    std::lock_guard<std::mutex> lock(mtx);
    if (active_preproc < max_preproc) { active_preproc++; return true; }
    return false;
}

void ResourceManager::releaseDealerSlot() {
    std::lock_guard<std::mutex> lock(mtx);
    if (active_preproc > 0) active_preproc--;
}