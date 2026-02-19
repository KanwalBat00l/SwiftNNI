#include "ConfigManager.hpp"
#include "KairosServer.hpp"
#include <csignal>
#include <iostream>
#include <getopt.h> // Include this for argument parsing


KairosServer* global_ptr = nullptr;

void handle_signal([[maybe_unused]] int sig) {
    if (global_ptr) global_ptr->stop();
}

#include <getopt.h> // Include this for argument parsing

int main(int argc, char* argv[]) {
    // Default values
    std::string config_file = "config.cfg";
    std::string profile_file = "profile.cfg";

    // --- Command Line Argument Parsing ---
    int opt;
    while ((opt = getopt(argc, argv, "c:p:h")) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;
            case 'p':
                profile_file = optarg;
                break;
            case 'h':
            default:
                std::cout << "Usage: " << argv[0] << " [-c config_path] [-p profile_path]\n";
                return 0;
        }
    }

    signal(SIGINT, handle_signal);

    try {
        std::cout << "[Kairos] Loading System Config: " << config_file << std::endl;
        SystemConfig sys = ConfigManager::loadSystemConfig(config_file);
        
        std::cout << "[Kairos] Loading Model Profiles: " << profile_file << std::endl;
        auto profiles = ConfigManager::loadProfiles(profile_file);

        // ... (Rest of the server initialization stays the same) ...
        KairosServer server(sys, profiles);
        global_ptr = &server;
        
        server.start();

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}