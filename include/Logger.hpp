#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <fstream>
#include "Types.hpp"

/**
 * @class Logger
 * @brief Thread-safe CSV logging for SwiftNNI job traces.
 * Uses a persistent file handle to prevent data loss under high concurrency.
 */
class Logger {
public:
    Logger(const std::string& filename);
    // Destructor to ensure file is closed properly
    ~Logger();

    /**
     * @brief Records the lifecycle and results of an inference or pre-processing job.
     */
    void logJob(const Job& j);

    // Getters for the server audit thread
    int getRCount() { return r_count.load(); }
    int getPCount() { return p_count.load(); }

private:
    std::string filename;
    std::mutex mtx;
    std::ofstream log_stream; // Persistent file handle

    std::atomic<int> r_count{0}; // Total real inference jobs completed
    std::atomic<int> p_count{0}; // Total pre-processing tasks completed
};