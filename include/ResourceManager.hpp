#pragma once
#include <vector>
#include <mutex>
#include <set>
#include <string>

/**
 * @class ResourceManager
 * @brief Manages CPU core partitioning (Buckets) and Network Ports.
 */
class ResourceManager {
public:
    ResourceManager(int total_cores, int port_base, int port_range, int max_preproc, bool enable_pinning);

    /**
     * @brief Reserves a specific core for a Dealer (pre-processing).
     * @return Physical core ID or -1 if none available.
     */
    int acquireDealerCore();

    /**
     * @brief Reserves a set of cores for Inference.
     * @return Vector of physical core IDs.
     */
    std::vector<int> acquireInferenceCores(int count);
    std::vector<int> acquireCoresElastic(int requested);

    /**
     * @brief Frees cores back to the available pool.
     */
    void releaseCores(const std::vector<int>& cores);
    
    /**
     * @brief Port management.
     */
    int acquirePort();
    void releasePort(int port);

    /**
     * @brief Pre-processing concurrency throttling.
     */
    bool hasCapacityForDealer();
    void releaseDealerSlot();

    /**
     * @brief Helper to convert core vectors to taskset-compatible strings.
     */
    std::string coresToString(const std::vector<int>& cores);

private:
    std::mutex mtx;
    bool pinning_enabled;
    
    // Core Pools (Buckets)
    std::vector<int> dealer_pool;
    std::vector<int> inference_pool;
    std::set<int> busy_cores;

    // Network Ports
    std::set<int> available_ports;
    
    // Throttling
    int max_preproc;
    int active_preproc;
};