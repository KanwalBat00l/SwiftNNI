#pragma once
#include "Types.hpp"
#include "ResourceManager.hpp"
#include "FileManager.hpp"
#include "IScheduler.hpp"
#include "Logger.hpp"

#include <memory>
#include <map>
#include <atomic>
#include <mutex>

class SwiftServer {
public:
    SwiftServer(SystemConfig sys, std::map<std::string, ModelProfile> profiles);
    void start();
    void stop();

private:
    // Core loops
    void dispatcherLoop();    // Pops jobs and runs Mode 0
    void replenisherLoop();   // Watches for DIRTY files and runs Mode 2
    void listenerLoop();      // Accepts TCP requests from clients

    SystemConfig sys;
    std::map<std::string, ModelProfile> profiles;

    std::unique_ptr<ResourceManager> rm;
    std::unique_ptr<FileManager> fm;
    std::unique_ptr<IScheduler> scheduler;
    std::unique_ptr<Logger> logger;

    std::atomic<bool> running{true};
    int server_fd{-1};
};