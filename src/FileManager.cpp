#include "FileManager.hpp"
#include <mutex>
#include <chrono>
#include <algorithm>

/**
 * @brief Starts a new pre-processing task.
 * Pre-pends the model name for auditability (e.g. alexnet_1_f_170000000).
 */
std::string FileManager::initiateFile(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    
    auto now = std::chrono::high_resolution_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    
    // Auditable prefix
    std::string prefix = model_key + "_f_" + std::to_string(micros);

    file_map[model_key].push_back({prefix, FileStatus::CREATING});
    return prefix;
}

void FileManager::setReady(const std::string& model_key, const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_map.find(model_key) == file_map.end()) return;

    for (auto& entry : file_map[model_key]) {
        if (entry.prefix == prefix) {
            entry.status = FileStatus::READY;
            return;
        }
    }
}

/**
 * @brief Thread-safe resource acquisition.
 * Finds a READY file and immediately marks it IN_USE to prevent double-allocation.
 */
std::string FileManager::acquireFile(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_map.find(model_key) == file_map.end()) return "";

    for (auto& entry : file_map[model_key]) {
        if (entry.status == FileStatus::READY) {
            entry.status = FileStatus::IN_USE;
            return entry.prefix;
        }
    }
    return "";
}

void FileManager::releaseFile(const std::string& model_key, const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_map.find(model_key) == file_map.end()) return;

    auto& vec = file_map[model_key];
    vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const FileEntry& e) {
        return e.prefix == prefix;
    }), vec.end());
}

int FileManager::getActiveCount(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_map.find(model_key) == file_map.end()) return 0;
    return static_cast<int>(file_map[model_key].size());
}