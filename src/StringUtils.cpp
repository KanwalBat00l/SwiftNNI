#include "StringUtils.hpp"
#include <algorithm>

/**
 * @brief Builds the shell command.
 * Logic: If 'core_range' is empty, it detects and removes the 'taskset -c' instruction.
 */
std::string StringUtils::buildCommand(std::string tmpl, 
                                     const std::string& snni_dir,
                                     const std::string& model, 
                                     int batch, 
                                     int port, 
                                     const std::string& file_prefix,
                                     const std::string& server_ip,
                                     const std::string& core_range) {
    
    auto replaceAll = [](std::string& s, const std::string& search, const std::string& val) {
        size_t pos = 0;
        while ((pos = s.find(search, pos)) != std::string::npos) {
            s.replace(pos, search.length(), val);
            pos += val.length();
        }
    };

    // --- STEP 1: Handle the taskset instruction ---
    if (core_range.empty()) {
        // Find the taskset instruction and remove it to let OS handle scheduling
        std::string target = "taskset -c {CORE_RANGE} ";
        size_t pos = tmpl.find(target);
        if (pos != std::string::npos) {
            tmpl.erase(pos, target.length());
        } else {
            // If the user used a different format, just clear the placeholder
            replaceAll(tmpl, "{CORE_RANGE}", "");
        }
    } else {
        // Pinning is active, replace with physical IDs
        replaceAll(tmpl, "{CORE_RANGE}", core_range);
    }

    // --- STEP 2: Replace remaining placeholders ---
    replaceAll(tmpl, "{SNNI_DIR}", snni_dir);
    replaceAll(tmpl, "{MODEL}", model);
    replaceAll(tmpl, "{BATCH}", std::to_string(batch));
    replaceAll(tmpl, "{PORT}", std::to_string(port));
    replaceAll(tmpl, "{FILE}", file_prefix);
    replaceAll(tmpl, "{SERVER_IP}", server_ip);

    return tmpl;
}