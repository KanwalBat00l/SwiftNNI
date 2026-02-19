#pragma once
#include <memory>
#include <string>
#include <stdexcept>

#include "IScheduler.hpp"
#include "FCFSScheduler.hpp"
#include "SJFScheduler.hpp"
#include "EDFScheduler.hpp"
#include "LSTScheduler.hpp"
#include "SAScheduler.hpp"
#include "DQNScheduler.hpp"
#include "CustomScheduler.hpp"

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
        else if (mode == "EDF")    return std::make_unique<EDFScheduler>();
        else if (mode == "LST")    return std::make_unique<LSTScheduler>();
        else if (mode == "SA")     return std::make_unique<SAScheduler>(sa_temp, sa_cooling, sa_max_iter, sa_window, sa_min_retrigger_ms, total_cores);
        else if (mode == "DQN")    return std::make_unique<DQNScheduler>(weights_path, total_cores, total_memory_mb);
        else if (mode == "CUSTOM") return std::make_unique<CustomScheduler>();

        throw std::runtime_error("Unknown scheduler mode: " + mode);
    }
};
