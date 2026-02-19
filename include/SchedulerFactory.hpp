#pragma once
#include <memory>
#include <string>
#include "FCFSScheduler.hpp"
#include "SJFScheduler.hpp"

class SchedulerFactory {
public:
    static std::unique_ptr<IScheduler> create(const std::string& mode, double aging_factor) {
        if (mode == "FCFS") {
            return std::make_unique<FCFSScheduler>();
        } else if (mode == "SJF") {
            return std::make_unique<SJFScheduler>(aging_factor);
        } else {
            throw std::runtime_error("SwiftNNI Error: Unknown scheduler mode: " + mode);
        }
    }
};