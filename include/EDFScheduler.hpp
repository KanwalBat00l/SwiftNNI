/**
* Earliest Deadline First (EDF): Prioritizes based on absolute deadline ($A + L$). Effective for general workloads but fails to consider the actual compute time of the models.
* Heuristic:  Score=ArrivalTime+SLO
* Goal: Minimize "Maximum Lateness" by finishing the most urgent deadlines first.
*/
#pragma once
#include "IScheduler.hpp"
#include <list>
#include <mutex>

class EDFScheduler : public IScheduler {
public:
    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    std::optional<Job> popReadyJob(FileManager& fm, ResourceManager& rm, 
                                   std::map<std::string, ModelProfile>& profiles, 
                                   double total_mem_gb) override {
        std::lock_guard<std::mutex> lock(mtx);
        
        // Sort the list by absolute deadline before processing
        queue.sort([](const Job& a, const Job& b) {
            return (a.arrival_ts + a.requested_slo_ms) < (b.arrival_ts + b.requested_slo_ms);
        });

        auto it = queue.begin();
        while (it != queue.end()) {
            std::string key = it->model + "_" + std::to_string(it->batch);
            ModelProfile& prof = profiles.at(key);

            // Resource Handshake
            SystemSnapshot snap = SystemMonitor::takeSnapshot();
            if (snap.mem_used_gb + (prof.inf_mem_mb/1024.0) <= total_mem_gb * 0.95) {
                std::string file = fm.acquireFile(key);
                if (!file.empty()) {
                    std::vector<int> cores = rm.acquireInferenceCores(prof.threads);
                    if (!cores.empty()) {
                        Job j = *it; j.assigned_file = file; j.assigned_cores = cores;
                        queue.erase(it);
                        return j;
                    }
                    fm.setReady(key, file);
                }
            }
            ++it;
        }
        return std::nullopt;
    }

    size_t size() override { std::lock_guard<std::mutex> lock(mtx); return queue.size(); }

private:
    std::list<Job> queue;
    std::mutex mtx;
};