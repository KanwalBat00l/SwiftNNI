#include "SJFScheduler.hpp"
#include <chrono>

SJFScheduler::SJFScheduler(double aging_factor) : aging_factor(aging_factor) {}

void SJFScheduler::push(const Job& j) {
    std::lock_guard<std::mutex> lock(mtx);
    queue.push_back(j);
}

size_t SJFScheduler::size() {
    std::lock_guard<std::mutex> lock(mtx);
    return queue.size();
}

std::optional<Job> SJFScheduler::pop() {
    std::lock_guard<std::mutex> lock(mtx);
    if (queue.empty()) return std::nullopt;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();

    auto best_it = queue.end();
    double best_score = 1e18; // Smaller is better

    for (auto it = queue.begin(); it != queue.end(); ++it) {
        // SJF + Aging Calculation: 
        // Score = (Expected Inference Time) - (Wait Time * Aging Factor)
        // We assume ModelProfiles are handled by the Dispatcher, 
        // but the scheduler calculates priority.
        
        long wait_time = now - it->arrival_ts;
        // Note: In Iteration 7, we will pass actual profiles here for precise SJF.
        // For now, we use a simplified priority score.
        double score = (double)it->requested_slo_ms - (wait_time * aging_factor);

        if (score < best_score) {
            best_score = score;
            best_it = it;
        }
    }

    if (best_it != queue.end()) {
        Job selected = *best_it;
        queue.erase(best_it);
        return selected;
    }
    return std::nullopt;
}