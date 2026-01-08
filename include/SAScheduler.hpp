#pragma once
#include "IScheduler.hpp"
#include <cmath>
#include <random>
#include <chrono>
#include <vector>

class SAScheduler : public IScheduler {
public:
    SAScheduler() : needsOptimization(false) {}

    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
        needsOptimization = true; 
    }

protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles, [[maybe_unused]] double total_mem_gb) override {
        if (!needsOptimization || queue.size() < 2) return;

        // Convert list window to vector for random access swaps
        size_t window_size = std::min(queue.size(), (size_t)15);
        std::vector<Job> seq;
        auto it = queue.begin();
        for(size_t i=0; i < window_size; ++it, ++i) seq.push_back(*it);

        // SA Parameters
        double temp = 100.0;
        double cooling = 0.92;
        int max_steps = 50; 
        double best_cost = calculateCost(seq, profiles);
        std::vector<Job> best_seq = seq;

        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, window_size - 1);
        std::uniform_real_distribution<double> coin(0.0, 1.0);

        while (temp > 1.0 && max_steps-- > 0) {
            size_t a = dist(rng), b = dist(rng);
            if (a == b) continue;

            std::swap(seq[a], seq[b]);
            double new_cost = calculateCost(seq, profiles);
            double delta = new_cost - best_cost;

            if (delta < 0 || (std::exp(-delta / temp) > coin(rng))) {
                if (new_cost < best_cost) {
                    best_cost = new_cost;
                    best_seq = seq;
                }
            } else {
                std::swap(seq[a], seq[b]); // Revert
            }
            temp *= cooling;
        }

        // Apply best found permutation back to the front of the list
        auto q_it = queue.begin();
        for (auto& job : best_seq) { *q_it = job; ++q_it; }
        needsOptimization = false;
    }

private:
    bool needsOptimization;

    double calculateCost(const std::vector<Job>& seq, std::map<std::string, ModelProfile>& profiles) {
        double cost = 0;
        long timeline = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

        for (const auto& j : seq) {
            long duration = profiles.at(j.model + "_" + std::to_string(j.batch)).dynamic_inf_ms.load();
            timeline += duration;
            long deadline = j.arrival_ts + j.requested_slo_ms;
            long tardiness = std::max(0L, timeline - deadline);
            if (tardiness > 0) cost += 1000.0 + (tardiness * tardiness);
        }
        return cost;
    }
};