#pragma once

#include "Types.hpp"
#include <map>
#include <string>
#include <vector>

/**
 * @class ConfigManager
 * @brief Utility to parse the SwiftNNI configuration and profile files.
 */
class ConfigManager {
public:
    /**
     * @brief Parses config.cfg into SystemConfig struct.
     */
    static SystemConfig loadSystemConfig(const std::string& filename);

    /**
     * @brief Parses profile.cfg into a map of model_batch keys to profiles.
     */
    static std::map<std::string, ModelProfile> loadProfiles(const std::string& filename);

    /**
     * @brief Helper to trim whitespace and control characters.
     */
    static std::string trim(const std::string& s);

private:
    /**
     * @brief Helper to split CSV lines.
     */
    static std::vector<std::string> split(const std::string& s, char delimiter);
};