#pragma once
#include <string>
#include <map>
#include <mutex>
#include "Types.hpp"

class FileManager {
public:
    FileStatus getStatus(const std::string& model_key);
    std::string initiatePreproc(const std::string& model_key);
    void setReady(const std::string& model_key, const std::string& filename);
    
    // Restored to the original destructive acquire
    std::string acquireFile(const std::string& model_key);

    void deleteFile(const std::string& filename, const std::string& snni_dir);
    void markDirty(const std::string& model_key);

private:
    std::mutex mtx;
    struct FileInfo {
        std::string filename;
        FileStatus status = FileStatus::DIRTY;
    };
    std::map<std::string, FileInfo> file_states;
};