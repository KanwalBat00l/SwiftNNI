#pragma once

#include "Types.hpp"
#include <string>

/**
 * @class SystemMonitor
 * @brief Handles telemetry gathering (CPU, RAM) and hybrid Energy Auditing.
 *
 * Supports three modes of energy tracking:
 * 1. RAPL (Real-time hardware registers)
 * 2. Slurm sacct (Fallback for clusters like Snellius with restricted hardware access)
 * 3. FU-15: Linear Power Model (Fallback when both RAPL and Slurm unavailable)
 */
class SystemMonitor {
public:
    /**
     * @brief Captures a full snapshot of current system performance.
     * @return SystemSnapshot struct with load, memory, and energy data.
     */
    static SystemSnapshot takeSnapshot();

    /**
     * @brief FU-15: Linear Power Model fallback for energy estimation.
     * Used when RAPL=0 and Slurm=0.
     * Energy = (P_idle + p_J * P_dynamic_per_core) * TAT_seconds
     * @param active_cores Number of cores assigned to the job
     * @param tat_ms Turnaround time in milliseconds
     * @param p_idle Idle power consumption in watts (default 100W)
     * @param p_dynamic_per_core Per-core dynamic power in watts (default 10W)
     * @return Estimated energy in microjoules
     */
    static long estimateEnergyLinearModel(int active_cores, long tat_ms,
                                           double p_idle = 100.0,
                                           double p_dynamic_per_core = 10.0);

private:

    static double getCpuLoad();
    static double getMemUsedGB();
    static double getTotalMemGB();
    /**
     * @brief High-resolution energy tracking via hardware RAPL.
     * @return Energy in microjoules, or 0 if access is denied.
     */
    static long getEnergyFromRAPL();

    /**
     * @brief Fallback energy tracking via Slurm accounting.
     * @return Energy in microjoules, converted from sacct strings.
     */
    static long getEnergyFromSlurm();

    /**
     * @brief Helper to convert Slurm strings (e.g., "1.2K", "500J") to microjoules.
     */
    static long parseEnergyString(std::string energy_str);
};
