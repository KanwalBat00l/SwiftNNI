#include "ConfigManager.hpp"
#include "SwiftServer.hpp"
#include <csignal>
#include <iostream>
#include <getopt.h>

SwiftServer* global_ptr = nullptr;

void handle_signal(int sig) {
    if (global_ptr) {
        std::cout << "\n[Swift] Received signal (" << sig << "), shutting down..." << std::endl;
        global_ptr->stop();
    }
}

int main(int argc, char* argv[]) {
    std::string config_file = "config.cfg";
    std::string profile_file = "profile.cfg";

    int opt;
    while ((opt = getopt(argc, argv, "c:p:h")) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'p': profile_file = optarg; break;
            case 'h':
            default:
                std::cout << "Usage: " << argv[0] << " [-c config_path] [-p profile_path]\n";
                return 0;
        }
    }

    // Capture Ctrl+C for graceful shutdown
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    try {
        std::cout << "[Swift] Loading System Config: " << config_file << std::endl;
        SystemConfig sys = ConfigManager::loadSystemConfig(config_file);
        
        std::cout << "[Swift] Loading Model Profiles: " << profile_file << std::endl;
        auto profiles = ConfigManager::loadProfiles(profile_file);

        // Instantiate the SwiftNNI Server
        SwiftServer server(sys, profiles);
        global_ptr = &server;
        
        std::cout << "[Swift] Server initialized. Mode: " << sys.scheduler_mode << std::endl;
        server.start();

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Swift] Shutdown complete." << std::endl;
    return 0;
}