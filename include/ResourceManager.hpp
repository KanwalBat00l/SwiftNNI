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
    ResourceManager(int requested_cores, int port_base, int port_range, int max_preproc, bool enable_pinning);

    /**
     * @brief Reserves a specific core for a Dealer (pre-processing).
     */
    int acquireDealerCore();

    /**
     * @brief Reserves a set of cores for Inference.
     */
    std::vector<int> acquireInferenceCores(int count);
    std::vector<int> acquireCoresElastic(int requested);

    /**
     * @brief Frees cores back to the available pool.
     */
    void releaseCores(const std::vector<int>& cores);
    
    /**
     * @brief Port management using Round-Robin entropy.
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

    // --- Port Management (Fixed for Iteration 7.0) ---
    std::vector<int> available_ports_vec; // Sequential list for Round-Robin
    std::set<int> busy_ports;             // Currently active ports
    size_t next_port_idx;                 // Needle for Port Entropy

    // Throttling
    int max_preproc;
    int active_preproc;
};