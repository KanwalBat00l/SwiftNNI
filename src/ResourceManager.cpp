#include "ResourceManager.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

ResourceManager::ResourceManager(int total_cores, int port_base, int port_range, int max_preproc)
    : total_cores(total_cores), used_threads(0), 
      next_port_idx(0), max_preproc(max_preproc), active_preproc(0) {
    
    // Initialize port pool
    for (int i = 0; i < port_range; ++i) {
        port_pool.push_back(port_base + i);
    }
}

/**
 * Logic: Checks if adding 'count' threads exceeds the hardware limit.
 * SwiftNNI lets the OS handle mapping, we just perform the accounting.
 */
bool ResourceManager::acquireThreads(int count) {
    std::lock_guard<std::mutex> lock(mtx);
    if (used_threads + count <= total_cores) {
        used_threads += count;
        return true;
    }
    return false;
}

void ResourceManager::releaseThreads(int count) {
    std::lock_guard<std::mutex> lock(mtx);
    used_threads -= count;
    if (used_threads < 0) used_threads = 0;
}

/**
 * Thread-safe port acquisition with Round-Robin entropy.
 * Verifies with the Linux Kernel that the port is truly available.
 */
int ResourceManager::acquirePort() {
    std::lock_guard<std::mutex> lock(mtx);
    size_t pool_size = port_pool.size();

    for (size_t i = 0; i < pool_size; ++i) {
        size_t idx = (next_port_idx + i) % pool_size;
        int candidate = port_pool[idx];

        if (busy_ports.find(candidate) == busy_ports.end()) {
            if (isPortFree(candidate)) {
                busy_ports.insert(candidate);
                next_port_idx = (idx + 1) % pool_size;
                return candidate;
            }
        }
    }
    return -1; 
}

void ResourceManager::releasePort(int port) {
    std::lock_guard<std::mutex> lock(mtx);
    busy_ports.erase(port);
}

bool ResourceManager::isPortFree(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

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

// Pre-processing (Mode 2) throttles
bool ResourceManager::hasPreprocSlot() {
    std::lock_guard<std::mutex> lock(mtx);
    return active_preproc < max_preproc;
}

void ResourceManager::occupyPreprocSlot() {
    std::lock_guard<std::mutex> lock(mtx);
    active_preproc++;
}

void ResourceManager::releasePreprocSlot() {
    std::lock_guard<std::mutex> lock(mtx);
    active_preproc--;
    if (active_preproc < 0) active_preproc = 0;
}