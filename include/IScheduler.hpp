#ifndef ISCHEDULER_HPP
#define ISCHEDULER_HPP

#include "Types.hpp"
#include <optional>

/**
 * @class IScheduler
 * @brief Abstract interface for scheduling strategies (FCFS, SJF, LST).
 */
class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void push(const Job& j) = 0;
    
    /**
     * @brief Picks the best job based on the strategy.
     * @param ready_checker A function or object that checks if resources are ready.
     */
    virtual std::optional<Job> pop() = 0;
    virtual size_t size() = 0;
};

#endif