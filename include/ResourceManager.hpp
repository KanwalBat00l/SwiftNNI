#pragma once
#include <vector>
#include <mutex>
#include <set>
#include <string>

/**
 * @class ResourceManager
 * @brief Thread-counting and Port-management for SwiftNNI.
 * 
 * Tracks available "slots" (cores) and network ports without physical pinning.
 */
class ResourceManager {
public:
    ResourceManager(int total_cores, int port_base, int port_range, int max_preproc);

    // --- Thread Management ---
    /**
     * @brief Logical acquisition of thread slots.
     * @return true if 'count' threads are available, false otherwise.
     */
    bool acquireThreads(int count);
    void releaseThreads(int count);

    // --- Port Management ---
    /**
     * @brief Returns a free port using Round-Robin entropy and kernel probing.
     */
    int acquirePort();
    void releasePort(int port);

    // --- Pre-processing Throttling ---
    bool hasPreprocSlot();
    void occupyPreprocSlot();
    void releasePreprocSlot();

    int getUsedThreads() { std::lock_guard<std::mutex> lk(mtx); return used_threads; }
int getActivePreprocCount() { std::lock_guard<std::mutex> lk(mtx); return active_preproc; }

bool canAcquireThreads(int count) {
    std::lock_guard<std::mutex> lock(mtx);
    return (used_threads + count <= total_cores);
}

private:
    std::mutex mtx;

    // Threads
    int total_cores;
    int used_threads;

    // Ports
    std::vector<int> port_pool;
    std::set<int> busy_ports;
    size_t next_port_idx;

    // Pre-processing
    int max_preproc;
    int active_preproc;

    // Kernel helper
    bool isPortFree(int port);
};