#pragma once
#include <string>
#include <map>
#include <mutex>
#include "Types.hpp"

/**
 * @class FileManager
 * @brief Manages the lifecycle of single-use .vmfb files.
 * 
 * Flow: initiatePreproc (DIRTY -> GENERATING) 
 *       -> setReady (GENERATING -> READY)
 *       -> acquireFile (READY -> DIRTY + trigger next)
 */
class FileManager {
public:
    /**
     * @brief Checks if a model_batch has a file ready for inference.
     */
    FileStatus getStatus(const std::string& model_key);

    /**
     * @brief Reserves the "Generating" slot. Returns a unique filename.
     */
    std::string initiatePreproc(const std::string& model_key);

    /**
     * @brief Marks a specific filename as READY for a model.
     */
    void setReady(const std::string& model_key, const std::string& filename);

    /**
     * @brief Picks the ready file, moves status to DIRTY. 
     * @return The filename to be used by the inference server.
     */
    std::string acquireFile(const std::string& model_key);

    /**
     * @brief Physically deletes the file from disk after inference is done.
     */
    void deleteFile(const std::string& filename, const std::string& snni_dir);

private:
    std::mutex mtx;
    
    struct FileInfo {
        std::string filename;
        FileStatus status = FileStatus::DIRTY;
    };

    // Map: "model_batch" -> FileInfo
    std::map<std::string, FileInfo> file_states;
};