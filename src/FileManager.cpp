#include "FileManager.hpp"
#include <chrono>
#include <iostream>
#include <filesystem>

FileStatus FileManager::getStatus(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_states.find(model_key) == file_states.end()) return FileStatus::DIRTY;
    return file_states[model_key].status;
}

std::string FileManager::initiatePreproc(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    
    auto now = std::chrono::high_resolution_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    
    // Create unique name: e.g., "simc2_4_17000000.vmfb"
    std::string filename = model_key + "_" + std::to_string(micros) + ".vmfb";

    file_states[model_key].filename = filename;
    file_states[model_key].status = FileStatus::GENERATING;
    
    return filename;
}

void FileManager::setReady(const std::string& model_key, const std::string& filename) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_states[model_key].filename == filename) {
        file_states[model_key].status = FileStatus::READY;
    }
}

std::string FileManager::acquireFile(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    if (file_states.find(model_key) == file_states.end() || 
        file_states[model_key].status != FileStatus::READY) {
        return "";
    }

    std::string file_to_use = file_states[model_key].filename;
    
    // Move to DIRTY immediately so the next request triggers a new pre-proc
    file_states[model_key].status = FileStatus::DIRTY;
    file_states[model_key].filename = "";

    return file_to_use;
}

void FileManager::deleteFile(const std::string& filename, const std::string& snni_dir) {
    try {
        std::string full_path = snni_dir + "/" + filename;
        if (std::filesystem::exists(full_path)) {
            std::filesystem::remove(full_path);
        }
    } catch (...) {
        // Silently fail if file can't be deleted (e.g. permission or already gone)
    }
}