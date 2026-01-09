#pragma once
#include "IScheduler.hpp"

/**
 * @class CustomScheduler
 * @brief Simple placeholder for future experimental algorithms.
 */
class CustomScheduler : public IScheduler {
protected:
    void sortQueue([[maybe_unused]] std::map<std::string, ModelProfile>& profiles, 
    [[maybe_unused]] double total_mem_gb) override{
        // Default: FCFS behavior (no sorting needed as push appends to back)
    }
};