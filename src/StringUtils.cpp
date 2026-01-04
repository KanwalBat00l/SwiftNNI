#include "StringUtils.hpp"
#include <algorithm>

std::string StringUtils::buildCommand(std::string tmpl, 
                                     const std::string& snni_dir,
                                     const std::string& model, 
                                     int batch, 
                                     int port, 
                                     const std::string& file_prefix,
                                     const std::string& server_ip,
                                     const std::string& core_range) {
    
    // Internal lambda for multiple replacements
    auto replaceAll = [](std::string& s, const std::string& search, const std::string& val) {
        size_t pos = 0;
        while ((pos = s.find(search, pos)) != std::string::npos) {
            s.replace(pos, search.length(), val);
            pos += val.length();
        }
    };

    // 1. Handle Core Pinning Condition
    if (core_range.empty()) {
        // If no cores assigned, remove the taskset wrapper entirely
        std::string taskset_part = "taskset -c {CORE_RANGE} ";
        size_t task_pos = tmpl.find(taskset_part);
        if (task_pos != std::string::npos) {
            tmpl.erase(task_pos, taskset_part.length());
        } else {
            // Fallback: just clear the placeholder if the template is different
            replaceAll(tmpl, "{CORE_RANGE}", "");
        }
    } else {
        replaceAll(tmpl, "{CORE_RANGE}", core_range);
    }

    // 2. Standard Placeholder Replacements
    replaceAll(tmpl, "{SNNI_DIR}", snni_dir);
    replaceAll(tmpl, "{MODEL}", model);
    replaceAll(tmpl, "{BATCH}", std::to_string(batch));
    replaceAll(tmpl, "{PORT}", std::to_string(port));
    replaceAll(tmpl, "{FILE}", file_prefix);
    replaceAll(tmpl, "{SERVER_IP}", server_ip);

    return tmpl;
}