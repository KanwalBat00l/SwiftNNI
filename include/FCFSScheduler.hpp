#pragma once
#include "IScheduler.hpp"
#include <queue>
#include <mutex>

class FCFSScheduler : public IScheduler {
public:
    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push(j);
    }

    std::optional<Job> pop() override {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;
        Job j = queue.front();
        queue.pop();
        return j;
    }

    size_t size() override {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.size();
    }

private:
    std::queue<Job> queue;
    std::mutex mtx;
};