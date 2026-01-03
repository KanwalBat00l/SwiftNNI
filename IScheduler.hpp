#pragma once
#include "Types.hpp"
#include <optional>

class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void push(const Job& job) = 0;
    virtual std::optional<Job> pop() = 0;
    virtual size_t size() const = 0;
};