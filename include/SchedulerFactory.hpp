#pragma once
#include <memory>
#include <string>
#include <stdexcept>

// Core Interfaces
#include "IScheduler.hpp"

// Concrete Implementations (Only included here)
#include "FCFSScheduler.hpp"
#include "SJFScheduler.hpp"

/**
 * @class SchedulerFactory
 * @brief Handles the creation of scheduler objects based on configuration strings.
 * 
 * This decouples SAPPISServer from specific scheduler headers, allowing the system
 * to be extended with new algorithms without modifying the main server logic.
 */
class SchedulerFactory {
public:
    static std::unique_ptr<IScheduler> create(const std::string& mode, double aging_factor) {
        if (mode == "FCFS") {
            return std::make_unique<FCFSScheduler>();
        } else if (mode == "SJF") {
            return std::make_unique<SJFScheduler>(aging_factor);
        }
        
        throw std::runtime_error("SchedulerFactory: Unknown mode '" + mode + "'");
    }
};