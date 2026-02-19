#include "Logger.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>

/**
 * Q-5: Extended CSV Trace Schema (27 columns).
 * Delimiter: semicolon (;) per client requirement.
 *
 * Original 21 columns + 6 measurement columns:
 *   22: Exec_Time_ms      — Pure execution time (finish - start)
 *   23: Speedup_Ratio     — Baseline inference / actual execution (>1 = faster than expected)
 *   24: Throughput_Local   — 1000/actual_tat (jobs per second, per-job perspective)
 *   25: Original_Model     — Original model before negotiation (empty if no negotiation)
 *   26: Completion_Time    — Contract §4: Timestamp when J_on exits (= Finish_TS)
 *   27: Tardiness          — Contract §4: max(0, actual_tat - requested_slo_ms)
 */
Logger::Logger(const std::string& filename) : filename(filename) {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream file(filename, std::ios::app);
    if (file.tellp() == 0) {
        file << "Arrival_TS;Start_TS;Finish_TS;Model_Batch;Total_Wait_Time;"
             << "Cores_Assigned;CPU_Load;Mem_GB;Energy_uJ;"
             << "Requested_SLO;Actual_TAT;SLO_Diff;SLO_Met;Exit_Code;"
             << "Request_ID;T_Start_Pre;T_Fin_Pre;T_Start_On;"
             << "Theta_Est;Theta_Act;Penalty_Phi;"
             << "Exec_Time_ms;Speedup_Ratio;Throughput_Local;Original_Model;"
             << "Completion_Time;Tardiness\n";
    }
}

void Logger::logJob(const Job& j, int exit_code, const SystemSnapshot& snap) {
    long actual_tat = j.finish_ts - j.arrival_ts;
    long wait_time = j.start_ts - j.arrival_ts;
    long slo_diff = j.requested_slo_ms - actual_tat;
    int slo_met = (slo_diff >= 0) ? 1 : 0;

    // New: Execution time = finish - start (pure processing, excludes queue wait)
    long exec_time = j.finish_ts - j.start_ts;

    // New: Speedup ratio = baseline_inf / actual_exec (>1 means faster than baseline)
    // Note: theta_act already captures this ratio; speedup = 1/theta_act
    double speedup_ratio = (j.theta_act > 0.001) ? (1.0 / j.theta_act) : 0.0;

    // New: Per-job throughput perspective (jobs/sec if this were the only workload)
    double throughput_local = (actual_tat > 0) ? (1000.0 / (double)actual_tat) : 0.0;

    // Contract §4: Tardiness = max(0, actual_tat - requested_slo)
    long tardiness = std::max(0L, actual_tat - j.requested_slo_ms);

    // Core list string
    std::stringstream cores_ss;
    for(size_t i = 0; i < j.assigned_cores.size(); ++i) {
        cores_ss << j.assigned_cores[i] << (i == j.assigned_cores.size()-1 ? "" : ",");
    }

    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream file(filename, std::ios::app);
    if (!file.is_open()) return;

    // 27-column output with 3 decimal places for theta/metric values
    file << j.arrival_ts << ";"                              // 1: Arrival_TS
         << j.start_ts << ";"                                // 2: Start_TS
         << j.finish_ts << ";"                               // 3: Finish_TS
         << j.model << "_" << j.batch << ";"                 // 4: Model_Batch
         << wait_time << ";"                                 // 5: Total_Wait_Time
         << cores_ss.str() << ";"                            // 6: Cores_Assigned
         << snap.cpu_load << ";"                             // 7: CPU_Load
         << snap.mem_used_gb << ";"                          // 8: Mem_GB
         << snap.energy_uj << ";"                            // 9: Energy_uJ
         << j.requested_slo_ms << ";"                        // 10: Requested_SLO
         << actual_tat << ";"                                // 11: Actual_TAT
         << slo_diff << ";"                                  // 12: SLO_Diff
         << slo_met << ";"                                   // 13: SLO_Met
         << exit_code << ";"                                 // 14: Exit_Code
         << j.request_id << ";"                              // 15: Request_ID
         << j.t_start_pre << ";"                             // 16: T_Start_Pre
         << j.t_fin_pre << ";"                               // 17: T_Fin_Pre
         << j.t_start_on << ";"                              // 18: T_Start_On
         << std::fixed << std::setprecision(3)
         << j.theta_est << ";"                               // 19: Theta_Est
         << j.theta_act << ";"                               // 20: Theta_Act
         << j.penalty_phi << ";"                             // 21: Penalty_Phi
         << exec_time << ";"                                 // 22: Exec_Time_ms
         << std::fixed << std::setprecision(4)
         << speedup_ratio << ";"                             // 23: Speedup_Ratio
         << throughput_local << ";"                           // 24: Throughput_Local
         << j.original_model << ";"                           // 25: Original_Model
         << j.finish_ts << ";"                               // 26: Completion_Time (= Finish_TS)
         << tardiness << "\n";                               // 27: Tardiness
}
