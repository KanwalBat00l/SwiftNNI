#pragma once
#include <string>

class StringUtils {
public:
    static std::string buildCommand(std::string tmpl, 
                                   const std::string& snni_dir,
                                   const std::string& model, 
                                   int batch, 
                                   int port, 
                                   const std::string& file_prefix,
                                   const std::string& server_ip = "127.0.0.1") {
        
        auto replace = [](std::string& s, const std::string& search, const std::string& replaceValue) {
            size_t pos = 0;
            while ((pos = s.find(search, pos)) != std::string::npos) {
                s.replace(pos, search.length(), replaceValue);
                pos += replaceValue.length();
            }
        };

        replace(tmpl, "{SNNI_DIR}", snni_dir);
        replace(tmpl, "{MODEL}", model);
        replace(tmpl, "{BATCH}", std::to_string(batch));
        replace(tmpl, "{PORT}", std::to_string(port));
        replace(tmpl, "{FILE}", file_prefix);
        replace(tmpl, "{SERVER_IP}", server_ip);

        return tmpl;
    }
};