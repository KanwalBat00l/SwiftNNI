#pragma once
#include "IScheduler.hpp"

/**
 * @class CustomScheduler
 * @brief Placeholder for any new scheduling technique you might wish to implement.
 */
class CustomScheduler : public IScheduler {
public:
    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    std::optional<Job> popReadyJob(FileManager& fm, ResourceManager& rm, 
                                   std::map<std::string, ModelProfile>& profiles, 
                                   double total_mem_gb) override {
        // TODO: Implement Sliding Window and Cost-Function based selection here.
        // For now, it acts as a simple FCFS.
        return std::nullopt; 
    }

    size_t size() override { return queue.size(); }

private:
    std::list<Job> queue;
    std::mutex mtx;
};