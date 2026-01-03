#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include "Types.hpp"

class Logger {
public:
    Logger(const std::string& filename) : filename(filename) {
        std::lock_guard<std::mutex> lock(mtx);
        std::ofstream file(filename, std::ios::app);
        if (file.tellp() == 0) {
            file << "Arrival_TS;Start_TS;Finish_TS;Model_Batch;Requested_SLO;Actual_TAT;SLO_Diff;SLO_Met;Exit_Code\n";
        }
    }

    void logJob(const Job& j, int exit_code) {
        long actual_tat = j.finish_ts - j.arrival_ts;
        long slo_diff = j.requested_slo_ms - actual_tat;
        int slo_met = (slo_diff >= 0) ? 1 : 0;

        std::lock_guard<std::mutex> lock(mtx);
        std::ofstream file(filename, std::ios::app);
        file << j.arrival_ts << ";"
             << j.start_ts << ";"
             << j.finish_ts << ";"
             << j.model << "_" << j.batch << ";"
             << j.requested_slo_ms << ";"
             << actual_tat << ";"
             << slo_diff << ";"
             << slo_met << ";"
             << exit_code << "\n";
    }

private:
    std::string filename;
    std::mutex mtx;
};