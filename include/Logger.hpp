#pragma once
#include <string>
#include <mutex>
#include "Types.hpp"

/**
 * @class Logger
 * @brief Thread-safe CSV logging for SwiftNNI job traces.
 */
class Logger {
public:
    Logger(const std::string& filename);
    
    /**
     * @brief Records the lifecycle and results of an inference or pre-processing job.
     */
    void logJob(const Job& j);

private:
    std::string filename;
    std::mutex mtx;
};