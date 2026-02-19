#include "Logger.hpp"
#include <fstream>
#include <iomanip>

/**
 * SwiftNNI Trace Schema (10 Columns).
 * Delimiter: semicolon (;)
 */
Logger::Logger(const std::string& filename) : filename(filename) {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream file(filename, std::ios::app);
    
    // Write header if the file is new
    if (file.tellp() == 0) {
        file << "Arrival_TS;Start_TS;Finish_TS;Wait_Time;Exec_Time;"
             << "Type;Model_Batch;Threads;Port;Exit_Code\n";
    }
}

void Logger::logJob(const Job& j) {
    // Basic timing calculations
    long wait_time = j.start_ts - j.arrival_ts;
    long exec_time = j.finish_ts - j.start_ts;

    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream file(filename, std::ios::app);
    if (!file.is_open()) return;

    // Output formatted CSV line
    file << j.arrival_ts << ";"                      // 1: Arrival Time (ms)
         << j.start_ts << ";"                        // 2: Start Time (ms)
         << j.finish_ts << ";"                       // 3: Finish Time (ms)
         << wait_time << ";"                         // 4: Time spent in queue
         << exec_time << ";"                         // 5: Pure execution time
         << j.type << ";"                            // 6: r (request), a (advance), p (preproc)
         << j.model << "_" << j.batch << ";"         // 7: Target workload
         << j.assigned_threads << ";"                // 8: OMP_NUM_THREADS
         << j.assigned_port << ";"                   // 9: TCP Port
         << j.exit_code << "\n";                     // 10: Process return code
    
    file.flush();
}