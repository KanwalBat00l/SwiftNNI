/**
* Least Slack Time (LST):** Calculates $Slack = (A + L) - (t + e_i)$. This is the primary heuristic for SAPPIS as it identifies "Zero-Slack" jobs that must be dispatched immediately to prevent failure.
* Heuristic: Slack=(Arrival+SLO)−(CurrentTime+ExecutionTime)
* Goal: Identify "Zero-Slack" jobs that have exactly enough time to finish before the deadline.
*/
#pragma once
#include "IScheduler.hpp"
#include <list>
#include <mutex>
#include <chrono>

class LSTScheduler : public IScheduler {
public:
    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    std::optional<Job> popReadyJob(FileManager& fm, ResourceManager& rm, 
                                   std::map<std::string, ModelProfile>& profiles, 
                                   double total_mem_gb) override {
        std::lock_guard<std::mutex> lock(mtx);
        
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();

        // Calculate Slack and sort
        // Priority: Smallest Slack first
        queue.sort([&](const Job& a, const Job& b) {
            long slack_a = (a.arrival_ts + a.requested_slo_ms) - (now + profiles.at(a.model+"_"+std::to_string(a.batch)).dynamic_inf_ms.load());
            long slack_b = (b.arrival_ts + b.requested_slo_ms) - (now + profiles.at(b.model+"_"+std::to_string(b.batch)).dynamic_inf_ms.load());
            return slack_a < slack_b;
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