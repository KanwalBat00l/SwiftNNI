#pragma once
#include <memory>
#include <string>
#include <stdexcept>

// Core Interfaces
#include "IScheduler.hpp"

// Concrete Implementations (Only included here)
#include "FCFSScheduler.hpp"
#include "SJFScheduler.hpp"
#include "EDFScheduler.hpp"
#include "LSTScheduler.hpp"
#include "SAScheduler.hpp"
#include "DQNScheduler.hpp"

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
        if (mode == "FCFS") return std::make_unique<FCFSScheduler>();
        else if (mode == "SJF")  return std::make_unique<SJFScheduler>(aging_factor);
        else if (mode == "EDF")  return std::make_unique<EDFScheduler>();
        else if (mode == "LST")  return std::make_unique<LSTScheduler>();
        else if (mode == "SA")  return std::make_unique<SAScheduler>();
        else if (mode == "DQN")  return std::make_unique<DQNScheduler>();
        else if (mode == "CUSTOM") return std::make_unique<CustomScheduler>();
        
        throw std::runtime_error("SchedulerFactory: Unknown mode '" + mode + "'");
    }
};