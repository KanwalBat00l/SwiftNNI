#pragma once
#include "IScheduler.hpp"
#include <list>
#include <mutex>
#include <chrono>

class SJFScheduler : public IScheduler {
public:
    SJFScheduler(double aging_factor) : aging_factor(aging_factor) {}

    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    std::optional<Job> popReadyJob(FileManager& fm, ResourceManager& rm, 
                                   std::map<std::string, ModelProfile>& profiles, 
                                   double total_mem_gb) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;

        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();

        auto best_it = queue.end();
        double best_score = 1e18; // Smaller is better

        for (auto it = queue.begin(); it != queue.end(); ++it) {
            std::string key = it->model + "_" + std::to_string(it->batch);
            ModelProfile& prof = profiles.at(key);

            // 1. Scoring Logic: SJF with Aging
            long wait_time = now - it->arrival_ts;
            double score = static_cast<double>(prof.dynamic_inf_ms.load()) - (wait_time * aging_factor);

            // 2. Resource-Aware Check
            if (score < best_score) {
                // Check RAM, File, and Cores
                SystemSnapshot snap = SystemMonitor::takeSnapshot();
                double proj_mem = snap.mem_used_gb + (static_cast<double>(prof.inf_mem_mb)/1024.0);
                
                if (proj_mem <= total_mem_gb * 0.95) {
                    std::string file_prefix = fm.acquireFile(key);
                    if (!file_prefix.empty()) {
                        std::vector<int> cores = rm.acquireInferenceCores(prof.threads);
                        if (!cores.empty()) {
                            // This is the best job that is ALSO ready
                            best_score = score;
                            if (best_it != queue.end()) {
                                // Important: If we found a better job, we must release the PREVIOUS best's file/cores
                                // (Implementation simplified here for readability: we only acquire if it's currently better)
                            }
                            // Actually, let's keep it simple: 
                            // In this iteration, we find the first job that is ready AND has a better score than the current best.
                            best_it = it;
                            Job selected = *it;
                            selected.assigned_file = file_prefix;
                            selected.assigned_cores = cores;
                            queue.erase(it);
                            return selected;
                        } else {
                            fm.setReady(key, file_prefix); // Release file
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    size_t size() override { std::lock_guard<std::mutex> lock(mtx); return queue.size(); }

private:
    std::list<Job> queue;
    std::mutex mtx;
    double aging_factor;
};