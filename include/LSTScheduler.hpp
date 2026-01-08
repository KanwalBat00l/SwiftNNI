#pragma once
#include "IScheduler.hpp"
#include <chrono>

class LSTScheduler : public IScheduler {
protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles, 
                   [[maybe_unused]] double total_mem_gb) override {
        
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();

        queue.sort([&](const Job& a, const Job& b) {
            // Fetch dynamic expected execution times
            auto& profA = profiles.at(a.model + "_" + std::to_string(a.batch));
            auto& profB = profiles.at(b.model + "_" + std::to_string(b.batch));

            // Slack = (Absolute Deadline) - (Current Time + Expected Execution Time)
            long slackA = (a.arrival_ts + a.requested_slo_ms) - (now + profA.dynamic_inf_ms.load());
            long slackB = (b.arrival_ts + b.requested_slo_ms) - (now + profB.dynamic_inf_ms.load());
            
            return slackA < slackB;
        });
    }
};