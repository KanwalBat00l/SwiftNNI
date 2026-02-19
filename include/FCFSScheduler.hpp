#pragma once
#include "IScheduler.hpp"

class FCFSScheduler : public IScheduler {
protected:
void sortQueue([[maybe_unused]] std::map<std::string, ModelProfile>& profiles, 
    [[maybe_unused]] double total_mem_gb) override  {
        // FCFS is already sorted by arrival_ts (order in list)
    }
};