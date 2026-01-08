#pragma once
#include "Types.hpp"
#include "FileManager.hpp"
#include "ResourceManager.hpp"
#include "SystemMonitor.hpp"
#include <optional>
#include <list>
#include <mutex>
#include <map>

class IScheduler {
public:
    virtual ~IScheduler() = default;

    /**
     * @brief Adds a job. Marked VIRTUAL to allow SAScheduler to override.
     */
    virtual void push(const Job& j) {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    virtual std::optional<Job> pop() { return std::nullopt; }

    /**
     * @brief Shared Bypass Logic.
     */
    virtual std::optional<Job> popReadyJob(
        FileManager& fm, 
        std::map<std::string, ModelProfile>& profiles,
        double total_mem_gb
    ) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;

        sortQueue(profiles, total_mem_gb);

        auto it = queue.begin();
        while (it != queue.end()) {
            std::string key = it->model + "_" + std::to_string(it->batch);
            std::string file_prefix = fm.acquireFile(key);
            
            if (!file_prefix.empty()) {
                Job selected = *it;
                selected.assigned_file = file_prefix;
                queue.erase(it);
                return selected;
            }
            ++it;
        }
        return std::nullopt;
    }

    virtual size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.size();
    }

protected:
    virtual void sortQueue(std::map<std::string, ModelProfile>& profiles, double total_mem_gb) = 0;

    std::list<Job> queue;
    std::mutex mtx;
};