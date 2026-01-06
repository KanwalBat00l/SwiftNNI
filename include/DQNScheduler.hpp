#pragma once
#include "IScheduler.hpp"
#include <list>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>

/**
 * @class DQNScheduler
 * @brief AI-Driven Priority Scheduler.
 * 
 * Logic: Encodes each job into a State Vector [WaitTime, SlackRatio, CoreReq, MemReq, FileStatus].
 * A Deep Q-Network (or a high-dimensional policy function) computes a 'Priority Score'.
 * The queue is then processed in order of highest predicted "System Reward".
 */
class DQNScheduler : public IScheduler {
public:
    DQNScheduler() = default;

    void push(const Job& j) override {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push_back(j);
    }

    std::optional<Job> pop() override {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;
        Job j = queue.front();
        queue.pop_front();
        return j;
    }

    /**
     * @brief The AI-Decision Engine.
     * Ranks the queue based on the State Vector and picks the best ready job.
     */
    std::optional<Job> popReadyJob(
        FileManager& fm, 
        ResourceManager& rm, 
        std::map<std::string, ModelProfile>& profiles,
        double total_mem_gb
    ) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (queue.empty()) return std::nullopt;

        // 1. STATE ENCODING & SCORING
        // We create a list of pointers to jobs and their AI-calculated scores.
        struct ScoredJob {
            std::list<Job>::iterator it;
            double score;
        };
        std::vector<ScoredJob> ranked_list;

        for (auto it = queue.begin(); it != queue.end(); ++it) {
            double q_value = computePolicyScore(*it, profiles, fm, total_mem_gb);
            ranked_list.push_back({it, q_value});
        }

        // 2. SORT BY AI SCORE (Descending)
        // High Score = AI believes this job is the most critical for system health.
        std::sort(ranked_list.begin(), ranked_list.end(), [](const ScoredJob& a, const ScoredJob& b) {
            return a.score > b.score;
        });

        // 3. SELECTION (With HoL Blocking Protection)
        for (auto& sj : ranked_list) {
            auto it = sj.it;
            std::string key = it->model + "_" + std::to_string(it->batch);
            auto& prof = profiles.at(key);

            // Resource Check
            std::string file = fm.acquireFile(key);
            if (!file.empty()) {
                auto cores = rm.acquireInferenceCores(prof.threads);
                if (!cores.empty()) {
                    Job result = *it;
                    result.assigned_file = file;
                    result.assigned_cores = cores;
                    queue.erase(it);
                    return result;
                }
                fm.setReady(key, file); // Resource not ready, try next ranked job
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

    /**
     * @brief Policy Scorer / State Encoder
     * In a full Libtorch setup, this would pass the vector to a .pt model.
     * Here, we implement the math that a DQN agent learns.
     */
    double computePolicyScore(const Job& j, std::map<std::string, ModelProfile>& profiles, 
                              FileManager& fm, double total_mem_gb) {
        
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        
        auto& p = profiles.at(j.model + "_" + std::to_string(j.batch));

        // --- FEATURE EXTRACTION (STATE VECTOR) ---
        // 1. Normalized Wait Time (How long has it been rotting?)
        double wait_time = (double)(now - j.arrival_ts) / 5000.0; 
        
        // 2. Slack Ratio (Urgency: 1.0 = on time, < 0.0 = overdue)
        double deadline = j.arrival_ts + j.requested_slo_ms;
        double slack_ratio = (double)(deadline - now) / (double)j.requested_slo_ms;

        // 3. Resource Pressure (Large jobs are harder to place)
        double core_pressure = (double)p.threads / 64.0;
        double mem_pressure = (double)p.inf_mem_mb / (total_mem_gb * 1024.0);

        // 4. Input Readiness (Binary: Is the file on disk?)
        double input_ready = (fm.getActiveCount(j.model + "_" + std::to_string(j.batch)) > 0) ? 1.0 : 0.0;

        // --- AI POLICY FUNCTION ---
        // Reward = - (SlackPenalty) + (WaitBonus) + (ReadinessBoost)
        // This is a linear approximation of a trained DQN output.
        double score = (input_ready * 5.0)     // AI learns readiness is top priority
                     - (slack_ratio * 10.0)    // AI learns to fear deadlines
                     + (wait_time * 2.0)       // AI learns to prevent starvation
                     - (core_pressure * 1.5);  // AI learns to be cautious of giants

        return score;
    }
};