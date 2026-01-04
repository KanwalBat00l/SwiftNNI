#pragma once

#include "Types.hpp"
#include "ResourceManager.hpp"
#include "FileManager.hpp"
#include "IScheduler.hpp"
#include "Logger.hpp"

// Standard Library Headers
#include <memory>  // REQUIRED for std::unique_ptr
#include <map>     // REQUIRED for std::map
#include <string>  // REQUIRED for std::string
#include <atomic>  // REQUIRED for std::atomic

/**
 * @class SAPPISServer
 * @brief The main orchestration engine for SAPPIS.
 * 
 * Manages the lifecycle of background threads (Dispatcher, Replenisher) 
 * and handles incoming client connections via the Listener loop.
 */
class SAPPISServer {
public:
    SAPPISServer(SystemConfig sys, std::map<std::string, ModelProfile> profiles);
    void start();
    void stop();

private:
    // Core Processing Loops
    void dispatcherLoop();
    void replenisherLoop();
    void listenerLoop();

    // Feedback Loop for Dynamic Profiling
    void updateDynamicMetrics(const std::string& key, long observed_inf, long observed_pre);

    // System Components
    SystemConfig sys;
    std::map<std::string, ModelProfile> profiles;
    
    // Smart Pointers for Resource Management
    std::unique_ptr<ResourceManager> rm;
    std::unique_ptr<FileManager> fm;
    std::unique_ptr<IScheduler> scheduler;
    std::unique_ptr<Logger> logger;

    // Control State
    std::atomic<bool> running{true};
    int server_fd{-1};
};