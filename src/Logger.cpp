#include "Logger.hpp"
#include <fstream>
#include <sstream>

Logger::Logger(const std::string& filename) : filename(filename) {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream file(filename, std::ios::app);
    if (file.tellp() == 0) {
        file << "Arrival_TS;Start_TS;Finish_TS;Model_Batch;Wait_Time;"
             << "Cores_Assigned;CPU_Load;Mem_GB;Energy_uJ;"
             << "Requested_SLO;Actual_TAT;SLO_Diff;SLO_Met;Exit_Code\n";
    }
}

void Logger::logJob(const Job& j, int exit_code, const SystemSnapshot& snap) {
    long actual_tat = j.finish_ts - j.arrival_ts;
    long wait_time = j.start_ts - j.arrival_ts;
    long slo_diff = j.requested_slo_ms - actual_tat;
    int slo_met = (slo_diff >= 0) ? 1 : 0;

    // Convert core vector to string (e.g., "72,73,74")
    std::stringstream cores_ss;
    for(size_t i = 0; i < j.assigned_cores.size(); ++i) {
        cores_ss << j.assigned_cores[i] << (i == j.assigned_cores.size()-1 ? "" : ",");
    }

    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream file(filename, std::ios::app);
    if (!file.is_open()) return;

    file << j.arrival_ts << ";"
         << j.start_ts << ";"
         << j.finish_ts << ";"
         << j.model << "_" << j.batch << ";"
         << wait_time << ";"
         << cores_ss.str() << ";"      // New: Physical Core IDs
         << snap.cpu_load << ";"
         << snap.mem_used_gb << ";"
         << snap.energy_uj << ";"      // Hybrid RAPL/Slurm Energy
         << j.requested_slo_ms << ";"
         << actual_tat << ";"
         << slo_diff << ";"
         << slo_met << ";"
         << exit_code << "\n";
}