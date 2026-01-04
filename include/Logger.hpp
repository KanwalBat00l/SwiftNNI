#pragma once
#include <string>
#include <mutex>
#include "Types.hpp"

class Logger {
public:
    Logger(const std::string& filename);
    
    /**
     * @brief Records job completion and system telemetry to CSV.
     */
    void logJob(const Job& j, int exit_code, const SystemSnapshot& snap);

private:
    std::string filename;
    std::mutex mtx;
};