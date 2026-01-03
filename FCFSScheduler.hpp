#pragma once
#include "IScheduler.hpp"
#include "Types.hpp"
#include <queue>
#include <mutex>
#include <optional>

class FCFSScheduler : public IScheduler {
public:
    void push(const Job& job) override {
        std::lock_guard<std::mutex> lock(mtx);
        q.push(job);
    }

    std::optional<Job> pop() override {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.empty()) return std::nullopt;
        Job j = q.front();
        q.pop();
        return j;
    }

    size_t size() const override {
        std::lock_guard<std::mutex> lock(mtx);
        return q.size();
    }

private:
    std::queue<Job> q;
    mutable std::mutex mtx;
};