#pragma once
#include "IScheduler.hpp"
#include <list>
#include <mutex>

class SJFScheduler : public IScheduler {
public:
    SJFScheduler(double aging_factor);
    void push(const Job& j) override;
    std::optional<Job> pop() override;
    size_t size() override;

private:
    std::list<Job> queue;
    std::mutex mtx;
    double aging_factor;
};