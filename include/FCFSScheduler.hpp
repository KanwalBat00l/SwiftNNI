#pragma once
#include "IScheduler.hpp"

class FCFSScheduler : public IScheduler {
protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles) override {
        // No actual sorting needed if we just treat the list as FCFS, 
        // but for consistency with popReadyJob:
        queue.sort([](const Job& a, const Job& b) {
            return a.arrival_ts < b.arrival_ts;
        });
    }
};