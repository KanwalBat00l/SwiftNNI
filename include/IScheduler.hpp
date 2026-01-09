#pragma once
#include "Types.hpp"
#include "FileManager.hpp"
#include <optional>
#include <list>
#include <mutex>
#include <map>

class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void push(const Job& j) {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    // Default implementation to prevent abstract class errors
    virtual std::optional<Job> pop() { return std::nullopt; }

    /**
     * @brief The Unified Bypass Logic.
     * Note: Signature now includes total_mem_gb for the AI schedulers.
     */
    virtual std::optional<Job> popReadyJob(
        FileManager& fm, 
        std::map<std::string, ModelProfile>& profiles,
        double total_mem_gb
    ) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;

        // Step 1: Subclass-specific ranking
        sortQueue(profiles, total_mem_gb);

        // Step 2: Resource-aware selection
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

    // Needed for VFT calculation in the Listener
    long getTotalWorkVolume(std::map<std::string, ModelProfile>& profiles) {
        std::lock_guard<std::mutex> lock(mtx);
        long volume = 0;
        for (auto& j : queue) {
            std::string key = j.model + "_" + std::to_string(j.batch);
            volume += (profiles.at(key).dynamic_inf_ms.load() * profiles.at(key).threads);
        }
        return volume;
    }

protected:
    // Signature MUST match exactly in all subclasses
    virtual void sortQueue(std::map<std::string, ModelProfile>& profiles, double total_mem_gb) = 0;

    std::list<Job> queue;
    std::mutex mtx;
};