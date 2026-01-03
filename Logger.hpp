#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <iostream>
#include "Types.hpp"

class Logger {
public:
    Logger(const std::string& filename) : filename(filename) {
        std::lock_guard<std::mutex> lock(mtx);
        std::ofstream file(filename, std::ios::app);
        
        // Check if file is empty to write the header
        if (file.tellp() == 0) {
            file << "Arrival_TS;Start_TS;Finish_TS;Model_Batch;Wait_Time;"
                 << "Cores_Busy;File_Ready;CPU_Load;Mem_GB;Energy_uJ;"
                 << "Requested_SLO;Actual_TAT;SLO_Diff;SLO_Met;Exit_Code\n";
        }
    }

    /**
     * Records a completed job with full system telemetry.
     * @param j: The Job object containing timestamps and SLO info.
     * @param exit_code: RC from the process launcher.
     * @param cores_busy: Number of cores utilized at the moment of dispatch.
     * @param file_was_ready: 1 if file was in READY pool, 0 if dispatcher had to wait.
     * @param snap: Snapshot of CPU, RAM, and Energy usage.
     */
    void logJob(const Job& j, int exit_code, int cores_busy, int file_was_ready, const SystemSnapshot& snap) {
        long actual_tat = j.finish_ts - j.arrival_ts;
        long wait_time = j.start_ts - j.arrival_ts;
        long slo_diff = j.requested_slo_ms - actual_tat;
        int slo_met = (slo_diff >= 0) ? 1 : 0;

        std::lock_guard<std::mutex> lock(mtx);
        std::ofstream file(filename, std::ios::app);
        
        if (!file.is_open()) {
            std::cerr << "[Logger] Error: Could not open log file for writing." << std::endl;
            return;
        }

        file << j.arrival_ts << ";"
             << j.start_ts << ";"
             << j.finish_ts << ";"
             << j.model << "_" << j.batch << ";"
             << wait_time << ";"            // Wait time in queue
             << cores_busy << ";"           // System load at start
             << file_was_ready << ";"       // Preproc efficiency metric
             << snap.cpu_load << ";"        // Telemetry: CPU
             << snap.mem_used_gb << ";"     // Telemetry: RAM
             << snap.energy_uj << ";"       // Telemetry: Energy (RAPL)
             << j.requested_slo_ms << ";"   // Target SLO
             << actual_tat << ";"           // End-to-end Turnaround Time
             << slo_diff << ";"             // Slack (+ve is good, -ve is violation)
             << slo_met << ";"              // Binary success metric
             << exit_code << "\n";
             
        file.flush(); // Ensure data is written even if server crashes
    }

private:
    std::string filename;
    std::mutex mtx;
};