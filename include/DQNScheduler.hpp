#pragma once
#include "IScheduler.hpp"
#include <chrono>

class DQNScheduler : public IScheduler {
protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles, double total_mem_gb) override {
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();

        // Use stable_sort to preserve arrival order for equal Q-values
        queue.sort([&](const Job& a, const Job& b) {
            double qA = computePolicyScore(a, profiles, total_mem_gb, now);
            double qB = computePolicyScore(b, profiles, total_mem_gb, now);
            return qA > qB; // Descending: Higher Reward first
        });
    }

private:
    double computePolicyScore(const Job& j, std::map<std::string, ModelProfile>& profiles, 
                              double total_mem_gb, long now) {
        
        auto& p = profiles.at(j.model + "_" + std::to_string(j.batch));

        // State Encoding
        double wait_time = (double)(now - j.arrival_ts) / 5000.0; 
        double deadline = j.arrival_ts + j.requested_slo_ms;
        double slack_ratio = (double)(deadline - now) / (double)j.requested_slo_ms;
        double core_pressure = (double)p.threads / 64.0;
        double mem_pressure = (double)p.inf_mem_mb / (total_mem_gb * 1024.0);

        // Linear Policy (Proxy for DQN inference)
        double score = (-slack_ratio * 10.0) 
                     + (wait_time * 2.0) 
                     - (core_pressure * 1.5)
                     - (mem_pressure * 2.0);

        return score;
    }
};