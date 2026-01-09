#pragma once
#include "Types.hpp"
#include "ResourceManager.hpp"
#include "FileManager.hpp"
#include "IScheduler.hpp"
#include "Logger.hpp"
#include "ActiveJobTracker.hpp" 

#include <memory>
#include <map>
#include <atomic>
#include <mutex>
#include <fstream>

class SAPPISServer {
public:
    SAPPISServer(SystemConfig sys, std::map<std::string, ModelProfile> profiles);
    void start();
    void stop();

private:
    void dispatcherLoop();
    void replenisherLoop();
    void listenerLoop();

    void updateDynamicMetrics(const std::string& key, long observed_inf, long observed_pre);
    void saveDynamicProfile();

    SystemConfig sys;
    std::map<std::string, ModelProfile> profiles;
    std::mutex mtx_profiles;

    std::unique_ptr<ResourceManager> rm;
    std::unique_ptr<FileManager> fm;
    std::unique_ptr<IScheduler> scheduler;
    std::unique_ptr<Logger> logger;
    std::unique_ptr<ActiveJobTracker> job_tracker; // NEW

    std::atomic<bool> running{true};
    int server_fd{-1};
};