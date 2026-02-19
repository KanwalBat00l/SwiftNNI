#pragma once
#include "Types.hpp"
#include <map>
#include <string>

/**
 * @class ConfigManager
 * @brief Static utility to parse and sanitize configuration files.
 */
class ConfigManager {
public:
    static SystemConfig loadSystemConfig(const std::string& filename);
    static std::map<std::string, ModelProfile> loadProfiles(const std::string& filename);
    
    /**
     * @brief Removes control characters (\r, \n) and trims whitespace.
     */
    static std::string trim(const std::string& s);
};