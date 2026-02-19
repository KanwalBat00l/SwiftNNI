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
    virtual void push(const Job& j) {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    // Default implementation to prevent abstract class errors
    virtual std::optional<Job> pop() { return std::nullopt; }

    /**
     * @brief The Unified Bypass Logic with Triple Check (C-31/FU-4).
     *
     * FU-4: Centralized Triple Check verifies three conditions before dispatch:
     *   1. File — pre-processed data file is available
     *   2. Cores — sufficient inference cores can be allocated (if ResourceManager provided)
     *   3. Memory — system memory usage is below safety threshold (if ResourceManager provided)
     *
     * Note: Signature includes total_mem_gb for AI schedulers and memory check.
     *       ResourceManager* is optional for backward compatibility with tests.
     */
    virtual std::optional<Job> popReadyJob(
        FileManager& fm,
        std::map<std::string, ModelProfile>& profiles,
        double total_mem_gb,
        ResourceManager* rm = nullptr,
        double mem_safety_threshold = 0.90,
        double mem_reserve_gb = 2.0
    ) {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;

        // Step 1: Subclass-specific ranking
        sortQueue(profiles, total_mem_gb);

        // FU-4 Check 3: Pre-check memory once — if rm is provided, verify system memory
        bool mem_ok = true;
        if (rm) {
            SystemSnapshot snap = SystemMonitor::takeSnapshot();
            mem_ok = (snap.mem_used_gb < (snap.total_mem_gb * mem_safety_threshold - mem_reserve_gb));
            if (!mem_ok) return std::nullopt; // Memory pressure — block all dispatches
        }

        // Step 2: Resource-aware selection with Triple Check (C-31/FU-4)
        auto it = queue.begin();
        while (it != queue.end()) {
            std::string key = it->model + "_" + std::to_string(it->batch);

            // FU-4 Check 2: Core gap check (query-only, no acquisition)
            if (rm) {
                int target_cores = (it->scheduled_cores > 0) ? it->scheduled_cores : profiles.at(key).threads;
                if (!rm->hasFreeInferenceCores(target_cores)) {
                    ++it;
                    continue; // Skip this job — insufficient cores
                }
            }

            // Check 1: File availability
            std::string file_prefix = fm.acquireFile(key);
            if (file_prefix.empty()) { ++it; continue; }

            // All applicable checks passed — pop the job
            Job selected = *it;
            selected.assigned_file = file_prefix;
            queue.erase(it);
            return selected;
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