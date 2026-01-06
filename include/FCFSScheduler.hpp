#pragma once
#include "IScheduler.hpp"
#include <list> // Switched from queue to list to allow bypassing
#include <mutex>

/**
 * @class FCFSScheduler
 * @brief First-Come-First-Served scheduler with Resource-Aware Bypassing.
 * 
 * Logic:
 * - Jobs are stored in a list (oldest at the front).
 * - popReadyJob() iterates from front to back, returning the first job 
 *   that can be executed immediately based on File, Core, and RAM availability.
 */
class FCFSScheduler : public IScheduler {
public:
    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    /**
     * @brief Simple FIFO pop (blocks if resources aren't checked).
     */
    std::optional<Job> pop() override {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;
        Job j = queue.front();
        queue.pop_front();
        return j;
    }

    /**
     * @brief Deep Fix: Resource-Aware Bypassing.
     * Searches the queue for the oldest job whose resources are ready.
     */
    std::optional<Job> popReadyJob(
        FileManager& fm, 
        ResourceManager& rm, 
        std::map<std::string, ModelProfile>& profiles,
        double total_mem_gb
    ) override {
        std::lock_guard<std::mutex> lock(mtx);
        
        if (queue.empty()) return std::nullopt;

        auto it = queue.begin();
        while (it != queue.end()) {
            std::string key = it->model + "_" + std::to_string(it->batch);
            ModelProfile& prof = profiles.at(key);

            // 1. Memory Safety Check (Static info from profile vs dynamic snapshot)
            SystemSnapshot snap = SystemMonitor::takeSnapshot();
            double projected_mem = snap.mem_used_gb + (static_cast<double>(prof.inf_mem_mb) / 1024.0);
            
            // Allow 5% buffer for OS stability
            bool mem_ok = (projected_mem <= (total_mem_gb * 0.95));

            // 2. File Readiness Check
            std::string file_prefix = fm.acquireFile(key);
            
            if (!file_prefix.empty()) {
                // 3. Hardware Core Check
                std::vector<int> cores = rm.acquireInferenceCores(prof.threads);
                
                if (!cores.empty() && mem_ok) {
                    // SUCCESS: All three constraints met.
                    Job selected = *it;
                    selected.assigned_file = file_prefix;
                    selected.assigned_cores = cores;
                    
                    queue.erase(it); // Remove the job from the queue
                    return selected;
                } else {
                    // PARTIAL SUCCESS: We got a file but cores/RAM are busy.
                    // We must return the file to the READY pool for others to use.
                    fm.setReady(key, file_prefix);
                    if (!cores.empty()) rm.releaseCores(cores);
                }
            }
            
            // BYPASS LOGIC: If this job is blocked, move to the next oldest job.
            ++it;
        }

        return std::nullopt; // No jobs are currently runnable.
    }

    size_t size() override {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.size();
    }

private:
    std::list<Job> queue; // List allows us to erase from the middle
    std::mutex mtx;
};