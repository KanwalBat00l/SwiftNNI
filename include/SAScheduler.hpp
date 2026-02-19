#pragma once
#include "IScheduler.hpp"
#include <cmath>
#include <random>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>

/**
 * SA Scheduler — Simulated Annealing with multi-objective cost function.
 *
 * Per Developer_Contract.pdf client answers:
 *   C-12: Four-Dimensional Search (swap_jobs, shift_dwell_time, reverse_subseq, adjust_core_count)
 *   C-16: Multi-Objective Energy: Cost = Tardiness² + (10 × MemPenalty) + SlackBonus + CorePenalty
 *   C-17: Hybrid Event-Triggered: re-run on Request Arrival AND Job Completion, 50ms min interval
 *   C-18: Full-Queue Visibility: optimize entire queue up to depth 100
 *   Q-6:  Malleable Core Sizing — 4th perturbation dimension for core count optimization
 */
class SAScheduler : public IScheduler {
public:
    SAScheduler(double initial_temp = 100.0, double cooling_rate = 0.92,
                int max_iterations = 80, int window_size = 100,
                int min_retrigger_ms = 50,
                int total_cores = 24)
        : needsOptimization(false),
          initialTemp(initial_temp),
          coolingRate(cooling_rate),
          maxIterations(max_iterations),
          windowSize(window_size),
          minRetriggerMs(min_retrigger_ms),
          lastOptimizeTs(0),
          totalCores(total_cores > 0 ? total_cores : 24) {}

    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
        needsOptimization = true;
    }

    // C-17: Allow external trigger on job completion
    void notifyJobComplete() {
        std::lock_guard<std::mutex> lock(mtx);
        needsOptimization = true;
    }

protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles,
                   double total_mem_gb) override {
        if (!needsOptimization || queue.size() < 2) return;

        // C-17: Enforce minimum interval between re-optimizations
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        if (lastOptimizeTs > 0 && (now - lastOptimizeTs) < minRetriggerMs) return;

        // C-18: Full-queue visibility up to depth windowSize (default 100)
        size_t win = std::min(queue.size(), (size_t)windowSize);
        std::vector<Job> seq;
        seq.reserve(win);
        auto it = queue.begin();
        for (size_t i = 0; i < win; ++it, ++i) seq.push_back(*it);

        // SA core loop
        double temp = initialTemp;
        double current_cost = calculateCost(seq, profiles, total_mem_gb);
        double best_cost = current_cost;
        std::vector<Job> best_seq = seq;
        std::vector<Job> current_seq = seq;

        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, win - 1);
        std::uniform_real_distribution<double> coin(0.0, 1.0);
        std::uniform_int_distribution<int> perturbType(0, 3); // Q-6: 4 perturbation types

        int steps = maxIterations;
        while (temp > 1.0 && steps-- > 0) {
            int ptype = perturbType(rng);

            // C-12 + Q-6: Four-Dimensional Search
            if (ptype == 0) {
                // Dimension 1: swap_jobs — swap two random jobs
                size_t a = dist(rng), b = dist(rng);
                if (a == b) continue;
                std::swap(current_seq[a], current_seq[b]);
            } else if (ptype == 1 && win > 2) {
                // Dimension 2: shift_dwell_time — move a job to a different position
                size_t from = dist(rng);
                size_t to = dist(rng);
                if (from == to) continue;
                Job moved = current_seq[from];
                current_seq.erase(current_seq.begin() + from);
                current_seq.insert(current_seq.begin() + to, moved);
            } else if (ptype == 2) {
                // Dimension 3: reverse_subsequence — reverse a small subsequence
                size_t a = dist(rng), b = dist(rng);
                if (a > b) std::swap(a, b);
                if (a == b) continue;
                std::reverse(current_seq.begin() + a, current_seq.begin() + b + 1);
            } else {
                // Q-6/FU-1 Dimension 4: adjust_core_count — change a job's core allocation
                // FU-1: SA search space clamped to [1, 16] per client specification
                size_t idx = dist(rng);
                Job& target = current_seq[idx];
                std::string key = target.model + "_" + std::to_string(target.batch);
                int default_cores = profiles.at(key).threads;
                // Try core counts: half, default, double (clamped to [1, min(16, P)])
                int sa_max_cores = std::min(16, totalCores);
                std::uniform_int_distribution<int> coreDist(0, 2);
                int choice = coreDist(rng);
                int new_cores;
                if (choice == 0) {
                    new_cores = std::max(1, default_cores / 2);
                } else if (choice == 1) {
                    new_cores = default_cores;
                } else {
                    new_cores = std::min(default_cores * 2, sa_max_cores);
                }
                target.scheduled_cores = new_cores;
            }

            double new_cost = calculateCost(current_seq, profiles, total_mem_gb);
            double delta = new_cost - current_cost;

            if (delta < 0 || (std::exp(-delta / temp) > coin(rng))) {
                // Accept the move
                current_cost = new_cost;
                if (current_cost < best_cost) {
                    best_cost = current_cost;
                    best_seq = current_seq;
                }
            } else {
                // Revert: restore best known state on rejection
                current_seq = best_seq;
                current_cost = best_cost;
            }
            temp *= coolingRate;
        }

        // Apply best found permutation back to the front of the list
        auto q_it = queue.begin();
        for (auto& job : best_seq) { *q_it = job; ++q_it; }
        needsOptimization = false;
        lastOptimizeTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    }

private:
    bool needsOptimization;
    double initialTemp;
    double coolingRate;
    int maxIterations;
    int windowSize;
    int minRetriggerMs;  // C-17: minimum interval between re-optimizations
    long lastOptimizeTs; // C-17: timestamp of last optimization
    int totalCores;      // Q-6: Total system cores for malleable bounds

    /**
     * Multi-objective cost function (C-16 + Q-6):
     *   Cost = Tardiness² + (10 × MemPenalty) + SlackBonus + CoreOvercommitPenalty
     */
    double calculateCost(const std::vector<Job>& seq,
                         std::map<std::string, ModelProfile>& profiles,
                         double total_mem_gb) {
        double cost = 0;
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        long timeline = now;

        double mem_total_mb = total_mem_gb * 1024.0;
        double running_mem_mb = 0;
        int running_cores = 0; // Q-6: track total core usage

        for (size_t i = 0; i < seq.size(); ++i) {
            const Job& j = seq[i];
            std::string key = j.model + "_" + std::to_string(j.batch);
            const ModelProfile& prof = profiles.at(key);

            // Q-6: Use scheduled_cores if set, otherwise profile default
            int job_cores = (j.scheduled_cores > 0) ? j.scheduled_cores : prof.threads;

            // FU-3/FU-14: Amdahl's Law speedup model — T(p) = e_pre + (θ_c × e_on) / p^k_n
            // FU-14: k_n is per-model (replaces global k=0.8)
            long e_pre = prof.dynamic_pre_ms.load();
            long e_on = prof.dynamic_inf_ms.load();
            double theta_c = std::max(1.0, j.theta_est);
            double k_n = (double)prof.parallel_efficiency_k;
            double p_k = std::pow((double)std::max(1, job_cores), k_n);
            long duration = e_pre + (long)((theta_c * (double)e_on) / p_k);

            double job_mem = (double)prof.inf_mem_mb;

            timeline += duration;

            // 1. Tardiness² penalty
            long deadline = j.arrival_ts + j.requested_slo_ms;
            long tardiness = std::max(0L, timeline - deadline);
            if (tardiness > 0) {
                cost += (double)tardiness * (double)tardiness;
            }

            // 2. Memory pressure penalty (10 × MemPenalty)
            running_mem_mb += job_mem;
            if (mem_total_mb > 0 && running_mem_mb > mem_total_mb * 0.9) {
                double overflow = running_mem_mb - (mem_total_mb * 0.9);
                cost += overflow * 10.0;
            }
            if (duration > 0) {
                running_mem_mb -= job_mem * 0.5;
                if (running_mem_mb < 0) running_mem_mb = 0;
            }

            // 3. Slack urgency bonus
            long slack = deadline - now;
            double position_weight = 1.0 / (1.0 + (double)i);
            if (slack > 0) {
                cost += (double)slack * 0.01 * (1.0 - position_weight);
            }

            // Q-6 4. Core overcommit penalty — Σp_J must not exceed P
            running_cores += job_cores;
            if (running_cores > totalCores) {
                cost += (double)(running_cores - totalCores) * 100.0;
            }
            // Partial core release (jobs finish sequentially)
            if (duration > 0) {
                running_cores -= job_cores / 2;
                if (running_cores < 0) running_cores = 0;
            }

            // Q-3: Priority penalty — deprioritize jobs with φ=1
            if (j.penalty_phi > 0) {
                cost += 500.0; // Fixed penalty for negotiation-refused jobs
            }
        }

        return cost;
    }
};
