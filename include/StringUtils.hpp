#pragma once
#include <string>

class StringUtils {
public:
    static std::string buildCommand(std::string tmpl, 
                                   const std::string& snni_dir,
                                   const std::string& model, 
                                   int batch, 
                                   int port, 
                                   const std::string& filename,
                                   const std::string& server_ip,
                                   int threads) {
        auto replace = [&](const std::string& anchor, const std::string& val) {
            size_t pos;
            while ((pos = tmpl.find(anchor)) != std::string::npos)
                tmpl.replace(pos, anchor.length(), val);
        };

        replace("{SNNI_DIR}", snni_dir);
        replace("{MODEL}", model);
        replace("{BATCH}", std::to_string(batch));
        replace("{PORT}", std::to_string(port));
        replace("{FILE}", filename);
        replace("{SERVER_IP}", server_ip);
        replace("{THREADS}", std::to_string(threads));

        return tmpl;
    }
};