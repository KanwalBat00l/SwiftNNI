#pragma once

#include "Types.hpp"
#include <string>

/**
 * @class SystemMonitor
 * @brief Handles telemetry gathering (CPU, RAM) and hybrid Energy Auditing.
 * 
 * Supports two modes of energy tracking:
 * 1. RAPL (Real-time hardware registers)
 * 2. Slurm sacct (Fallback for clusters like Snellius with restricted hardware access)
 */
class SystemMonitor {
public:
    /**
     * @brief Captures a full snapshot of current system performance.
     * @return SystemSnapshot struct with load, memory, and energy data.
     */
    static SystemSnapshot takeSnapshot();

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
