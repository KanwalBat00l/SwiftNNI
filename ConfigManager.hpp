#pragma once
#include "Types.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>

class ConfigManager {
public:
    static SystemConfig loadSystemConfig(const std::string& filename);
    static std::map<std::string, ModelProfile> loadProfiles(const std::string& filename);
    static std::string trim(const std::string& s); // Just declaration
};