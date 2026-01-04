#include "ConfigManager.hpp"
#include "SAPPISServer.hpp"
#include <csignal>
#include <iostream>

SAPPISServer* global_ptr = nullptr;

void handle_signal([[maybe_unused]] int sig) {
    if (global_ptr) global_ptr->stop();
}

int main() {
    try {
        SystemConfig sys = ConfigManager::loadSystemConfig("config.cfg");
        auto profiles = ConfigManager::loadProfiles("profile.cfg");

        SAPPISServer server(sys, profiles);
        global_ptr = &server;
        signal(SIGINT, handle_signal);

        std::cout << "[SAPPIS] Starting Server Engine..." << std::endl;
        server.start(); // This blocks until shutdown

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}