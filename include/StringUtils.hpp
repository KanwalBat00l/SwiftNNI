#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <string>

/**
 * @class StringUtils
 * @brief Utility class for dynamic string manipulation and shell command construction.
 */
class StringUtils {
public:
    /**
     * @brief Builds a shell command from a template with placeholder replacement.
     * 
     * Handles conditional core pinning: if core_range is empty, the "taskset -c" 
     * prefix is automatically removed from the command.
     * 
     * @param tmpl The command template (e.g., "taskset -c {CORE_RANGE} ./cmd")
     * @param snni_dir Directory where the binary is located.
     * @param model Model name to replace {MODEL}.
     * @param batch Batch size to replace {BATCH}.
     * @param port Port to replace {PORT}.
     * @param file_prefix File prefix to replace {FILE}.
     * @param server_ip IP address to replace {SERVER_IP}.
     * @param core_range Comma-separated physical core IDs (e.g., "0,1,2").
     * @return The sanitized, ready-to-execute command string.
     */
    static std::string buildCommand(std::string tmpl, 
                                   const std::string& snni_dir,
                                   const std::string& model, 
                                   int batch, 
                                   int port, 
                                   const std::string& file_prefix,
                                   const std::string& server_ip = "127.0.0.1",
                                   const std::string& core_range = "");
};

#endif