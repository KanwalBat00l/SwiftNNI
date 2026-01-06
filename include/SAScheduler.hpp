#pragma once
#include "IScheduler.hpp"
#include <list>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <random>
#include <chrono>

/**
 * @class SAScheduler
 * @brief Simulated Annealing (SA) based Job Scheduler.
 * 
 * CATEGORY: Meta-Heuristic / Stochastic Optimization.
 * 
 * RATIONALE:
 * Uses a cooling schedule to reorder the job queue to minimize "Total Weighted Tardiness."
 * Optimized for SAPPIS to avoid high CPU overhead by only "thinking" when the queue changes.
 */
class SAScheduler : public IScheduler {
public:
    SAScheduler() : needsOptimization(false) {}

    /**
     * @brief Adds a job to the queue. Marks as 'dirty' so we re-optimize on next pop.
     */
    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
        needsOptimization = true; 
    }

    /**
     * @brief Standard FIFO pop (rarely used in SAPPIS).
     */
    std::optional<Job> pop() override {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;
        Job j = queue.front();
        queue.pop_front();
        return j;
    }

    /**
     * @brief Resource-Aware Pop. 
     * 1. Re-optimizes the order if a new job arrived.
     * 2. Scans the list for the most urgent job that has resources ready.
     */
    virtual std::optional<Job> popReadyJob(
        FileManager& fm, 
        ResourceManager& rm, 
        std::map<std::string, ModelProfile>& profiles,
        double total_mem_gb
    ) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;

        // OPTIMIZATION: Only run SA if the queue is "Dirty" and has multiple jobs.
        if (needsOptimization && queue.size() > 1) {
            runSA(profiles);
            needsOptimization = false; 
        }

        // RESOURCE SELECTION: Find first job that fits physical constraints.
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            std::string key = it->model + "_" + std::to_string(it->batch);
            auto& prof = profiles.at(key);

            // Constraint Check: File availability
            std::string file = fm.acquireFile(key);
            if (!file.empty()) {
                // Constraint Check: Core availability
                auto cores = rm.acquireInferenceCores(prof.threads);
                if (!cores.empty()) {
                    Job j = *it;
                    j.assigned_file = file;
                    j.assigned_cores = cores;
                    queue.erase(it);
                    return j; // Job is ready to execute!
                }
                // Backtrack: File was ready but cores weren't. Free the file.
                fm.setReady(key, file);
            }
        }
        return std::nullopt;
    }

    size_t size() override {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.size();
    }

private:
    std::list<Job> queue;
    std::mutex mtx;
    bool needsOptimization;

    /**
     * @brief The SA Optimization Engine.
     * Uses a fixed window and warm-start to keep overhead ~50 microseconds.
     */
    void runSA(std::map<std::string, ModelProfile>& profiles) {
        // Windowing: Only optimize the first 15 jobs to prevent O(n) lag.
        size_t window_size = std::min(queue.size(), (size_t)15);
        std::vector<Job> seq;
        auto it = queue.begin();
        for(size_t i=0; i < window_size; ++it, ++i) seq.push_back(*it);

        // Parameters: Lower iterations = faster, higher = better quality.
        double temp = 100.0;
        double cooling = 0.92;
        int max_steps = 60; 

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

            // Metropolis Rule: Accept better, or accept worse with probability.
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

        // Update the actual queue with our best sequence.
        auto q_it = queue.begin();
        for (auto& job : best_seq) { *q_it = job; ++q_it; }
    }

    /**
     * @brief Cost Function: Predicts Total Tardiness.
     */
    double calculateCost(const std::vector<Job>& seq, std::map<std::string, ModelProfile>& profiles) {
        double cost = 0;
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        long timeline = now;

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