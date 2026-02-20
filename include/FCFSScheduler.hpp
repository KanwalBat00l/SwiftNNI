#pragma once
#include "IScheduler.hpp"

class FCFSScheduler : public IScheduler {
    protected:
        // We leave the name out of 'profiles' to stop the warning
        void sortQueue(std::map<std::string, ModelProfile>& /*profiles*/) override {
            queue.sort([](const Job& a, const Job& b) {
                return a.arrival_ts < b.arrival_ts;
            });
        }
};