#pragma once
#include <vector>
#include <mutex>
#include <chrono>
#include "Types.hpp"

struct ActiveJobEntry {
    std::string file_prefix;
    long start_ts;
    long expected_duration;
    int threads;
};

class ActiveJobTracker {
public:
    void add(std::string prefix, long expected, int threads) {
        std::lock_guard<std::mutex> lock(mtx);
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        active_list.push_back({prefix, now, expected, threads});
    }

    void remove(std::string prefix) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto it = active_list.begin(); it != active_list.end(); ++it) {
            if (it->file_prefix == prefix) {
                active_list.erase(it);
                return;
            }
        }
    }

    /**
     * @brief Volume of remaining work for currently running jobs.
     */
    long getResidualWorkVolume() {
        std::lock_guard<std::mutex> lock(mtx);
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        long volume = 0;
        for (auto& aj : active_list) {
            long remaining = (aj.start_ts + aj.expected_duration) - now;
            if (remaining > 0) volume += (remaining * aj.threads);
        }
        return volume;
    }

private:
    std::vector<ActiveJobEntry> active_list;
    std::mutex mtx;
};