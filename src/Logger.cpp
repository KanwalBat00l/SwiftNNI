#include "Logger.hpp"
#include <iostream>
#include <iomanip>

/**
 * Constructor: Opens the file once and keeps it open for the duration of the server life.
 */
Logger::Logger(const std::string& filename) : filename(filename) {
    std::lock_guard<std::mutex> lock(mtx);
    
    // Open in append mode
    log_stream.open(filename, std::ios::app);

    if (!log_stream.is_open()) {
        std::cerr << "[Logger] FATAL ERROR: Could not open log file: " << filename << std::endl;
        return;
    }

    // Check if we need to write a header (if file size is 0)
    log_stream.seekp(0, std::ios::end);
    if (log_stream.tellp() == 0) {
        log_stream << "Arrival_TS;Start_TS;Finish_TS;Wait_Time;Exec_Time;"
                   << "Type;Model_Batch;Threads;Port;Exit_Code\n";
        log_stream.flush();
    }
}

/**
 * Destructor: Safely closes the file handle.
 */
Logger::~Logger() {
    if (log_stream.is_open()) {
        log_stream.close();
    }
}

void Logger::logJob(const Job& j) {
    // 1. Calculate timing metrics
    long wait_time = j.start_ts - j.arrival_ts;
    long exec_time = j.finish_ts - j.start_ts;

    // 2. Critical Section: Write to the persistent stream
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!log_stream.is_open()) return;

        log_stream << j.arrival_ts << ";"                      // 1
                   << j.start_ts << ";"                        // 2
                   << j.finish_ts << ";"                       // 3
                   << wait_time << ";"                         // 4
                   << exec_time << ";"                         // 5
                   << j.type << ";"                            // 6
                   << j.model << "_" << j.batch << ";"         // 7
                   << j.assigned_threads << ";"                // 8
                   << j.assigned_port << ";"                   // 9
                   << j.exit_code << "\n";                     // 10

        // Flush ensures data is committed to disk even if the server crashes later
        log_stream.flush();
    }

    // 3. Update atomics for the [AUDIT] console line
    if (j.type == 'r') r_count++;
    if (j.type == 'p') p_count++;
}