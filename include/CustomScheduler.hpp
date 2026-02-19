#pragma once
#include "IScheduler.hpp"
#include <chrono>

/**
 * @class CustomScheduler
 * @brief SPLS (State-Prioritized Least-Slack) algorithm (C-27).
 *
 * Combines LST (slack urgency) with system state awareness:
 *   priority = -slack + state_bonus
 * where state_bonus adjusts for current memory pressure.
 */
class CustomScheduler : public IScheduler {
protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles,
                   double total_mem_gb) override {
        if (queue.size() < 2) return;

        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        double mem_total_mb = total_mem_gb * 1024.0;

        queue.sort([&](const Job& a, const Job& b) {
            std::string keyA = a.model + "_" + std::to_string(a.batch);
            std::string keyB = b.model + "_" + std::to_string(b.batch);
            const ModelProfile& profA = profiles.at(keyA);
            const ModelProfile& profB = profiles.at(keyB);

            // Slack = deadline - (now + expected_inference)
            long slackA = (a.arrival_ts + a.requested_slo_ms) - (now + profA.dynamic_inf_ms.load());
            long slackB = (b.arrival_ts + b.requested_slo_ms) - (now + profB.dynamic_inf_ms.load());

            // State-aware memory bonus: penalize jobs that need more memory
            // when system is memory-constrained
            double memPenA = (mem_total_mb > 0) ? (double)profA.inf_mem_mb / mem_total_mb : 0.0;
            double memPenB = (mem_total_mb > 0) ? (double)profB.inf_mem_mb / mem_total_mb : 0.0;

            // SPLS score: lower slack = higher priority, adjusted by memory pressure
            double scoreA = (double)slackA + memPenA * 1000.0;
            double scoreB = (double)slackB + memPenB * 1000.0;

            return scoreA < scoreB; // Lower score = higher priority
        });
    }
};
