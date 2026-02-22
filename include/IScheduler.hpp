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

    virtual std::optional<Job> popReadyJob(
        FileManager& fm,
        ResourceManager& rm, // Pass the Resource Manager here
        std::map<std::string, ModelProfile>& profiles
    ) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;
    
        sortQueue(profiles);
    
        auto it = queue.begin();
        while (it != queue.end()) {
            std::string key = it->model + "_" + std::to_string(it->batch);
            int threads_needed = profiles[key].threads;
    
            // 1. Check threads without consuming them
            if (!rm.canAcquireThreads(threads_needed)) {
                ++it; 
                continue;
            }
    
            // 2. Check if file is ready
            std::string file = fm.acquireFile(key);
            if (file.empty()) {
                ++it; 
                continue;
            }
    
            // 3. Atomic success: We have threads and file. Consume threads now.
            rm.acquireThreads(threads_needed);
    
            Job selected = *it;
            selected.assigned_file = file;
            selected.assigned_threads = threads_needed;
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