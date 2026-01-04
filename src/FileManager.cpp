#include "FileManager.hpp"
#include <algorithm>

/**
 * Marks the beginning of a file creation process.
 * Generates a unique prefix to prevent file collisions between concurrent jobs.
 */
std::string FileManager::initiateFile(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    
    auto now = std::chrono::high_resolution_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    std::string prefix = "f_" + std::to_string(micros);

    file_map[model_key].push_back({prefix, FileStatus::CREATING});
    return prefix;
}

/**
 * Transitions a file from the 'Creating' state to 'Ready'.
 * This file is now visible to the Dispatcher for inference.
 */
void FileManager::setReady(const std::string& model_key, const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& entry : file_map[model_key]) {
        if (entry.prefix == prefix) {
            entry.status = FileStatus::READY;
            return;
        }
    }
}

/**
 * Searches for a READY file for a specific model-batch.
 * FIXED: Uses '==' for comparison instead of '='.
 */
std::string FileManager::acquireFile(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_map.find(model_key) == file_map.end()) return "";

    for (auto& entry : file_map[model_key]) {
        // FIX: Comparison operator '==' instead of assignment '='
        if (entry.status == FileStatus::READY) {
            entry.status = FileStatus::IN_USE;
            return entry.prefix;
        }
    }
    return "";
}

/**
 * Removes a file entry once the inference process and physical deletion are finished.
 */
void FileManager::releaseFile(const std::string& model_key, const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mtx);
    auto& vec = file_map[model_key];
    vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const FileEntry& e) {
        return e.prefix == prefix;
    }), vec.end());
}

int FileManager::getActiveCount(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    return static_cast<int>(file_map[model_key].size());
}