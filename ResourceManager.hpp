#pragma once
#include <mutex>
#include <set>
#include <atomic>

class ResourceManager {
public:
    ResourceManager(int total_cores, int port_base, int port_range, int max_preproc);

    // Cores
    bool tryAcquireCores(int count);
    void releaseCores(int count);
    int getFreeCores() const;

    // Ports
    int acquirePort();
    void releasePort(int port);

    // Preprocessing Throttling
    bool tryAcquirePreproc();
    void releasePreproc();

private:
    mutable std::mutex mtx;
    int total_cores;
    int used_cores;
    int port_base;
    int port_range;
    std::set<int> available_ports;
    std::set<int> busy_ports;

    // NEW: Throttling for memory-intensive Dealer tasks
    int max_preproc;
    int active_preproc;
};