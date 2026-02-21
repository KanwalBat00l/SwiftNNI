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
    file_states[model_key].status = FileStatus::DIRTY;
    file_states[model_key].filename = "";
    return file_to_use;
}

void FileManager::markDirty(const std::string& model_key) {
    std::lock_guard<std::mutex> lock(mtx);
    file_states[model_key].status = FileStatus::DIRTY;
    file_states[model_key].filename = "";
}

void FileManager::deleteFile(const std::string& filename, const std::string& snni_dir) {
    try {
        std::string full_path = snni_dir + "/" + filename;
        std::string s_path = full_path + "_server.dat";
        std::string c_path = full_path + "_client.dat";
        if (std::filesystem::exists(full_path)) std::filesystem::remove(full_path);
        if (std::filesystem::exists(s_path)) std::filesystem::remove(s_path);
        if (std::filesystem::exists(c_path)) std::filesystem::remove(c_path);
    } catch (...) {}
}