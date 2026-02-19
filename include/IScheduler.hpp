#pragma once
#include "Types.hpp"
#include "FileManager.hpp"
#include <list>
#include <mutex>
#include <optional>
#include <map>

class IScheduler {
public:
    virtual ~IScheduler() = default;

    virtual void push(const Job& j) {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    /**
     * @brief Picks the highest priority job that has a READY file.
     * Logic: Sort the queue, then iterate until we find a job we can run.
     */
    virtual std::optional<Job> popReadyJob(
        FileManager& fm,
        std::map<std::string, ModelProfile>& profiles
    ) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;

        // 1. Sort based on implementation (FCFS or SJF)
        sortQueue(profiles);

        // 2. Iterate to find a job with a READY file
        auto it = queue.begin();
        while (it != queue.end()) {
            std::string key = it->model + "_" + std::to_string(it->batch);

            // Check if file is ready
            std::string file = fm.acquireFile(key);
            if (file.empty()) {
                ++it; // File not ready, try next job (Bypass)
                continue;
            }

            // Job is ready!
            Job selected = *it;
            selected.assigned_file = file;
            queue.erase(it);
            return selected;
        }
        return std::nullopt;
    }

    virtual size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.size();
    }

protected:
    virtual void sortQueue(std::map<std::string, ModelProfile>& profiles) = 0;

    std::list<Job> queue;
    std::mutex mtx;
};