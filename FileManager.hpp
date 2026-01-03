#pragma once
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Types.hpp"


struct FileObject {
    std::string prefix;
    FileStatus status;
};

class FileManager {
public:
    // Returns the total count of files that are not 'USED'
    int getActiveCount(const std::string& model_key) {
        std::lock_guard<std::mutex> lk(mtx);
        return pool[model_key].size();
    }

    // Add a new entry in CREATING state
    std::string initiateFile(const std::string& model_key) {
        std::lock_guard<std::mutex> lk(mtx);
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string prefix = model_key + "_" + std::to_string(now);
        pool[model_key].push_back({prefix, FileStatus::CREATING});
        return prefix;
    }

    // Promote a file from CREATING to READY
    void setReady(const std::string& model_key, const std::string& prefix) {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& f : pool[model_key]) {
            if (f.prefix == prefix) { f.status = FileStatus::READY; break; }
        }
    }

    // Dispatcher picks a READY file and marks it IN_USE
    std::string acquireFile(const std::string& model_key) {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& f : pool[model_key]) {
            if (f.status == FileStatus::READY) {
                f.status = FileStatus::IN_USE;
                return f.prefix;
            }
        }
        return "";
    }

    // Remove file from tracking once inference is done
    void releaseFile(const std::string& model_key, const std::string& prefix) {
        std::lock_guard<std::mutex> lk(mtx);
        auto& v = pool[model_key];
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it->prefix == prefix) { v.erase(it); break; }
        }
    }

private:
    std::mutex mtx;
    std::map<std::string, std::vector<FileObject>> pool;
};