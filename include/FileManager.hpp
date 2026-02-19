#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include "Types.hpp"

/**
 * @class FileManager
 * @brief Manages the lifecycle of SHARK pre-processed data files.
 * 
 * Tracks files through states: CREATING -> READY -> IN_USE.
 * Each file is identified by a unique timestamped prefix to allow concurrency.
 */
class FileManager {
public:
    /**
     * @brief Reserves a slot and returns a unique prefix for a new Dealer task.
     */
    std::string initiateFile(const std::string& model_key);

    /**
     * @brief Moves a file from CREATING to READY state.
     */
    void setReady(const std::string& model_key, const std::string& prefix);

    /**
     * @brief Picks a READY file for inference and marks it IN_USE.
     */
    std::string acquireFile(const std::string& model_key);

    /**
     * @brief Final step: Removes the file from the tracking map.
     */
    void releaseFile(const std::string& model_key, const std::string& prefix);

    /**
     * @brief Returns the total count of files (Creating + Ready + InUse) for a model.
     */
    int getActiveCount(const std::string& model_key);

private:
    struct FileEntry {
        std::string prefix;
        FileStatus status;
    };

    std::mutex mtx;
    // Map: model_key -> vector of tracked file entries
    std::map<std::string, std::vector<FileEntry>> file_map;
};