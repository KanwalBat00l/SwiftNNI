#pragma once
#include "IScheduler.hpp"
#include <chrono>

class SJFScheduler : public IScheduler {
public:
    SJFScheduler(double aging_factor) : aging_factor(aging_factor) {}
protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles, 
    [[maybe_unused]] double total_mem_gb) override {
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        queue.sort([&](const Job& a, const Job& b) {
            auto& pA = profiles.at(a.model + "_" + std::to_string(a.batch));
            auto& pB = profiles.at(b.model + "_" + std::to_string(b.batch));
            double sA = (double)pA.dynamic_inf_ms.load() - ((now - a.arrival_ts) * aging_factor);
            double sB = (double)pB.dynamic_inf_ms.load() - ((now - b.arrival_ts) * aging_factor);
            return sA < sB;
        });
    }
private:
    double aging_factor;
};