#pragma once
#include "IScheduler.hpp"
#include <chrono>

class SJFScheduler : public IScheduler {
public:
    SJFScheduler(double aging_factor) : aging_factor(aging_factor) {}

protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles) override {
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();

        queue.sort([&](const Job& a, const Job& b) {
            std::string keyA = a.model + "_" + std::to_string(a.batch);
            std::string keyB = b.model + "_" + std::to_string(b.batch);

            // Priority Score = Static_Inf_Time - (Wait_Time * Aging)
            double sA = (double)profiles.at(keyA).inf_ms - ((now - a.arrival_ts) * aging_factor);
            double sB = (double)profiles.at(keyB).inf_ms - ((now - b.arrival_ts) * aging_factor);
            
            return sA < sB; // Lower score = higher priority
        });
    }

private:
    double aging_factor;
};