#pragma once
#include "Types.hpp"
#include "ResourceManager.hpp"
#include "FileManager.hpp"
#include "IScheduler.hpp"
#include "Logger.hpp"
#include "ActiveJobTracker.hpp" 

#include <memory>
#include <map>
#include <atomic>
#include <mutex>
#include <fstream>

class KairosServer {
public:
    KairosServer(SystemConfig sys, std::map<std::string, ModelProfile> profiles);
    void start();
    void stop();

private:
    void dispatcherLoop();
    void replenisherLoop();
    void listenerLoop();
    void telemetryLoop(); // Q-8: 100ms memory polling thread

    void updateDynamicMetrics(const std::string& key, long observed_inf, long observed_pre);
    void saveDynamicProfile();

    // Q-2: TTFR Timer 1 — measure TCP socket RTT to estimate θ_c
    double measureTTFR_Timer1(int client_sock);

    // Q-3: Negotiation State Machine — propose n_alt to weak client
    bool negotiateWithClient(int client_sock, const std::string& original_key,
                             double theta_c, std::string& out_model, int& out_batch,
                             int& out_penalty_phi);

    SystemConfig sys;
    std::map<std::string, ModelProfile> profiles;
    std::mutex mtx_profiles;

    std::unique_ptr<ResourceManager> rm;
    std::unique_ptr<FileManager> fm;
    std::unique_ptr<IScheduler> scheduler;
    std::unique_ptr<Logger> logger;
    std::unique_ptr<ActiveJobTracker> job_tracker; // NEW

    std::atomic<bool> running{true};
    int server_fd{-1};

    // Q-11: Real-time Ψ metric tracking
    std::atomic<long> psi_success_count{0};   // Σ χ_m (SLO-met count)
    std::atomic<long> psi_total_tat_ms{0};    // Σ TAT_m (total turnaround time in ms)
    std::atomic<long> psi_total_jobs{0};       // Total completed jobs

    // Throughput tracking
    std::atomic<long> throughput_total_completed{0};
    long throughput_start_ts{0};  // Set when server starts
};