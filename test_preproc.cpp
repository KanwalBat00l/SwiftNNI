#include <iostream>
#include <vector>
#include <filesystem> // Requires C++17
#include "ConfigManager.hpp"
#include "FileManager.hpp"
#include "StringUtils.hpp"
#include "ProcessLauncher.hpp"

namespace fs = std::filesystem;

int main() {
    try {
        // 1. Setup
        SystemConfig sys = ConfigManager::loadSystemConfig("config.cfg");
        auto profiles = ConfigManager::loadProfiles("profile.cfg");
        FileManager file_manager;

        std::cout << "[DEBUG] Iteration 3: Preprocessing Test" << std::endl;

        // 2. Target a specific model (e.g., hinet batch 1)
        std::string target_key = "hinet_1";
        if (profiles.find(target_key) == profiles.end()) {
            throw std::runtime_error("hinet_1 not found in profile.cfg");
        }
        ModelProfile profile = profiles[target_key];

        // 3. Simulate "Needs Replenishment" logic
        if (file_manager.getActiveCount(target_key) < profile.max_buffer) {
            std::cout << "[DEBUG] Buffer slot open for " << target_key << ". Starting Preproc..." << std::endl;

            // 4. Initiate unique file
            std::string unique_prefix = file_manager.initiateFile(target_key);
            std::cout << "[DEBUG] Generated Unique Prefix: " << unique_prefix << std::endl;

            // 5. Build Command
            std::string cmd = StringUtils::buildCommand(
                sys.preproc_cmd_template,
                sys.snni_dir,
                profile.model,
                profile.batch,
                sys.pre_base_port, // Usually use dealer ports
                unique_prefix
            );

            // 6. Run SHARK Dealer
            int rc = ProcessLauncher::runShellCommand(cmd);

            if (rc == 0) {
                std::cout << "[SUCCESS] Dealer exited successfully." << std::endl;
                file_manager.setReady(target_key, unique_prefix);
                
                // 7. Verify File Existence on Disk
                // SHARK appends _server.dat and _client.dat
                std::string expected_file = sys.snni_dir + "/" + unique_prefix + "_server.dat";
                if (fs::exists(expected_file)) {
                    std::cout << "[VERIFIED] Found file on disk: " << expected_file << std::endl;
                } else {
                    std::cout << "[ERROR] Binary reported success but " << expected_file << " not found!" << std::endl;
                }
            } else {
                std::cout << "[FAILURE] Dealer failed with exit code: " << rc << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }
    return 0;
}