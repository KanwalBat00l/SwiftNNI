#pragma once
#include "Types.hpp"
#include "FileManager.hpp"
#include "ResourceManager.hpp"
#include "SystemMonitor.hpp"
#include <optional>
#include <map>

/**
 * @class IScheduler
 * @brief Interface for resource-aware job scheduling.
 */
class IScheduler {
public:
    virtual ~IScheduler() = default;

    /**
     * @brief Adds a job to the queue.
     */
    virtual void push(const Job& j) = 0;

    /**
     * @brief Standard FIFO/Priority pop.
     */
    virtual std::optional<Job> pop() = 0;

    /**
     * @brief Resource-Aware Pop: Finds the oldest/highest priority job that 
     * has all dependencies (File, Cores, RAM) ready. 
     * Prevents Head-of-Line (HoL) blocking.
     */
    virtual std::optional<Job> popReadyJob(
        FileManager& fm, 
        ResourceManager& rm, 
        std::map<std::string, ModelProfile>& profiles,
        double total_mem_gb
    ) = 0;

    virtual size_t size() = 0;
};