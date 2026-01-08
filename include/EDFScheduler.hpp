#pragma once
#include "IScheduler.hpp"

/**
 * @class EDFScheduler
 * @brief Earliest Deadline First Scheduler.
 * 
 * Heuristic: Sorts jobs by their absolute deadline (Arrival + SLO).
 * Note: This implementation follows the new Template Method Pattern,
 * overriding only the ranking logic.
 */
class EDFScheduler : public IScheduler {
protected:
    /**
     * @brief Ranks the queue based on absolute deadlines.
     * @param profiles (Unused by EDF)
     * @param total_mem_gb (Unused by EDF)
     */
    void sortQueue([[maybe_unused]] std::map<std::string, ModelProfile>& profiles, 
                   [[maybe_unused]] double total_mem_gb) override {
        
        queue.sort([](const Job& a, const Job& b) {
            // Absolute Deadline = Arrival Time + Requested SLO
            long deadlineA = a.arrival_ts + a.requested_slo_ms;
            long deadlineB = b.arrival_ts + b.requested_slo_ms;
            
            return deadlineA < deadlineB;
        });
    }
};