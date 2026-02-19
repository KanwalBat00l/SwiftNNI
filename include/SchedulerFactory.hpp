#pragma once
#include <memory>
#include <string>
#include <stdexcept>

#include "IScheduler.hpp"
#include "FCFSScheduler.hpp"
#include "SJFScheduler.hpp"


class SchedulerFactory {
public:
    // Backward-compatible overload
    static std::unique_ptr<IScheduler> create(const std::string& mode, double aging_factor, const std::string& weights_path) {
        return create(mode, aging_factor, weights_path, 100.0, 0.92, 80, 100, 50, 0, 0);
    }

    static std::unique_ptr<IScheduler> create(const std::string& mode, double aging_factor,
                                               const std::string& weights_path,
                                               double sa_temp, double sa_cooling,
                                               int sa_max_iter, int sa_window,
                                               int sa_min_retrigger_ms = 50,
                                               int total_cores = 0,
                                               int total_memory_mb = 0) {
        if (mode == "FCFS")        return std::make_unique<FCFSScheduler>();
        else if (mode == "SJF")    return std::make_unique<SJFScheduler>(aging_factor);
        else // not required
        throw std::runtime_error("Unknown scheduler mode: " + mode);
    }
};
