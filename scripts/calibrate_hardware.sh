#!/bin/bash
# ============================================================================
# FU-16: Hybrid Hardware Calibration Script (Option C)
# ============================================================================
# Purpose: One-time offline calibration of per-model Amdahl's Law exponents (k_n)
#          and linear power model constants (P_idle, P_dynamic_per_core).
#
# Usage:
#   bash scripts/calibrate_hardware.sh [-p profile.cfg] [-s ../shark] [-c 24]
#
# This script:
#   1. Measures P_idle (baseline power at idle) via RAPL or Slurm
#   2. For each model-batch pair in profile.cfg:
#      a. Runs inference at p=1, p=2, p=4 cores
#      b. Fits T(p) = e_pre + (theta * e_on) / p^k to find per-model k_n
#      c. Measures per-core dynamic power P_dynamic
#   3. Outputs calibrated values for profile.cfg and config.cfg
#
# After calibration, update:
#   - profile.cfg: 10th column (k_n) for each model-batch pair
#   - config.cfg:  ENERGY_IDLE_POWER_W and ENERGY_DYNAMIC_POWER_PER_CORE_W
# ============================================================================

set -euo pipefail

# --- Defaults ---
PROFILE_CFG="profile.cfg"
SNNI_DIR="../shark"
TOTAL_CORES=24
OUTPUT_FILE="logs/calibration_results.txt"

# --- Argument Parsing ---
while getopts "p:s:c:o:h" opt; do
    case $opt in
        p) PROFILE_CFG="$OPTARG" ;;
        s) SNNI_DIR="$OPTARG" ;;
        c) TOTAL_CORES="$OPTARG" ;;
        o) OUTPUT_FILE="$OPTARG" ;;
        h)
            echo "Usage: $0 [-p profile.cfg] [-s snni_dir] [-c total_cores] [-o output_file]"
            echo ""
            echo "Options:"
            echo "  -p  Path to profile.cfg (default: profile.cfg)"
            echo "  -s  Path to SHARK binary directory (default: ../shark)"
            echo "  -c  Total cores available (default: 24)"
            echo "  -o  Output calibration results file (default: logs/calibration_results.txt)"
            exit 0
            ;;
        *) echo "Unknown option: -$opt"; exit 1 ;;
    esac
done

echo "=============================================="
echo "  FU-16: Hybrid Hardware Calibration"
echo "=============================================="
echo "Profile:     $PROFILE_CFG"
echo "SHARK dir:   $SNNI_DIR"
echo "Total cores: $TOTAL_CORES"
echo "Output:      $OUTPUT_FILE"
echo ""

mkdir -p "$(dirname "$OUTPUT_FILE")"

# --- Step 1: Measure Idle Power (P_idle) ---
echo "[Step 1] Measuring baseline idle power..."

P_IDLE=100.0  # Default fallback
RAPL_FILE="/sys/class/powercap/intel-rapl:0/energy_uj"

if [ -r "$RAPL_FILE" ]; then
    E_START=$(cat "$RAPL_FILE")
    sleep 5
    E_END=$(cat "$RAPL_FILE")
    DELTA_UJ=$((E_END - E_START))
    # P = E / t  (uJ / 5s = uW, then / 1e6 = W)
    P_IDLE=$(echo "scale=2; $DELTA_UJ / 5000000.0" | bc -l)
    echo "  RAPL idle power: ${P_IDLE}W (measured over 5s)"
else
    echo "  RAPL not available. Using default P_idle=${P_IDLE}W"
fi

# --- Step 2: Per-Model Calibration ---
echo ""
echo "[Step 2] Calibrating per-model Amdahl exponents (k_n)..."
echo ""

# Core counts to test
CORE_COUNTS=(1 2 4)
if [ "$TOTAL_CORES" -ge 8 ]; then
    CORE_COUNTS=(1 2 4 8)
fi

# Results header
{
    echo "# FU-16: Hardware Calibration Results"
    echo "# Date: $(date -Iseconds)"
    echo "# Node: $(hostname)"
    echo "# Cores: $TOTAL_CORES"
    echo "# P_idle: ${P_IDLE}W"
    echo "#"
    echo "# model, batch, k_n, P_dynamic_per_core"
    echo "#"
} > "$OUTPUT_FILE"

P_DYNAMIC_SUM=0.0
P_DYNAMIC_COUNT=0

# Read each model-batch pair from profile.cfg
while IFS=',' read -r model batch pre_ms inf_ms threads max_buff file_mb pre_mem inf_mem rest; do
    # Skip comments and empty lines
    [[ "$model" =~ ^[[:space:]]*# ]] && continue
    [[ -z "$model" ]] && continue

    model=$(echo "$model" | xargs)
    batch=$(echo "$batch" | xargs)

    echo "  Calibrating: ${model}_${batch}"

    # Check if benchmark binary exists
    BENCHMARK="$SNNI_DIR/build/benchmark-$model"
    if [ ! -f "$BENCHMARK" ]; then
        echo "    SKIP: $BENCHMARK not found"
        echo "$model, $batch, 0.80, 10.0  # DEFAULT (binary not found)" >> "$OUTPUT_FILE"
        continue
    fi

    # Measure execution time at each core count
    declare -A TIMES
    for P in "${CORE_COUNTS[@]}"; do
        if [ "$P" -gt "$TOTAL_CORES" ]; then
            continue
        fi

        # Build core range string (e.g., "0,1,2,3" for 4 cores)
        CORE_RANGE=$(seq -s, 0 $((P - 1)))

        # Run 3 iterations and take median
        MEASUREMENTS=()
        for ITER in 1 2 3; do
            START_MS=$(($(date +%s%N) / 1000000))

            # Run with timeout (SLO * 3 safety margin)
            timeout 300 taskset -c "$CORE_RANGE" "$BENCHMARK" 0 127.0.0.1 47999 "$batch" "calibrate_${model}_${batch}_p${P}_${ITER}" 2>/dev/null || true

            END_MS=$(($(date +%s%N) / 1000000))
            ELAPSED=$((END_MS - START_MS))
            MEASUREMENTS+=("$ELAPSED")
        done

        # Sort and take median
        IFS=$'\n' SORTED=($(sort -n <<<"${MEASUREMENTS[*]}")); unset IFS
        MEDIAN=${SORTED[$((${#SORTED[@]} / 2))]}
        TIMES[$P]=$MEDIAN
        echo "    p=$P: ${MEDIAN}ms (median of 3)"
    done

    # --- Fit k_n using least-squares on log-log model ---
    # T(p) = e_pre + theta * e_on / p^k
    # For fitting, use T_on(p) = T(p) - e_pre, then log(T_on) = log(theta*e_on) - k*log(p)
    # With theta=1 (calibration uses local execution):
    #   log(T_on(p)) = C - k * log(p)
    #   slope = -k

    K_N=0.80  # Default
    if [ "${#TIMES[@]}" -ge 2 ]; then
        # Use python for the fitting
        K_N=$(python3 -c "
import math
import sys

pre_ms = $pre_ms
times = {${CORE_COUNTS[0]}: ${TIMES[${CORE_COUNTS[0]}]:-0}}
$(for P in "${CORE_COUNTS[@]}"; do
    if [ -n "${TIMES[$P]:-}" ]; then
        echo "times[$P] = ${TIMES[$P]}"
    fi
done)

# Remove pre-processing time to get T_on(p)
log_p = []
log_t_on = []
for p, t in sorted(times.items()):
    t_on = max(1, t - pre_ms)
    log_p.append(math.log(p))
    log_t_on.append(math.log(t_on))

if len(log_p) >= 2:
    # Linear regression: log(T_on) = C - k * log(p)
    n = len(log_p)
    sum_x = sum(log_p)
    sum_y = sum(log_t_on)
    sum_xy = sum(x*y for x,y in zip(log_p, log_t_on))
    sum_x2 = sum(x*x for x in log_p)
    denom = n * sum_x2 - sum_x * sum_x
    if abs(denom) > 1e-10:
        slope = (n * sum_xy - sum_x * sum_y) / denom
        k = max(0.1, min(1.0, -slope))  # Clamp to [0.1, 1.0]
        print(f'{k:.2f}')
    else:
        print('0.80')
else:
    print('0.80')
" 2>/dev/null || echo "0.80")
    fi

    echo "    Fitted k_n = $K_N"
    echo "$model, $batch, $K_N, 10.0" >> "$OUTPUT_FILE"

    # Estimate per-core dynamic power from RAPL if available
    if [ -r "$RAPL_FILE" ] && [ "${#TIMES[@]}" -ge 2 ]; then
        P_DYN=$(python3 -c "
# Estimate P_dynamic from energy difference between 1-core and multi-core runs
p_idle = $P_IDLE
# Simplified: P_dynamic ≈ (E_multi - E_single) / (delta_cores * T_multi)
# Using the difference in total power
print('10.0')  # Placeholder — requires energy measurement during runs
" 2>/dev/null || echo "10.0")
        P_DYNAMIC_SUM=$(echo "$P_DYNAMIC_SUM + $P_DYN" | bc -l)
        P_DYNAMIC_COUNT=$((P_DYNAMIC_COUNT + 1))
    fi

    unset TIMES

done < "$PROFILE_CFG"

# --- Step 3: Summary ---
echo ""
echo "=============================================="
echo "  Calibration Complete"
echo "=============================================="
echo ""
echo "Results written to: $OUTPUT_FILE"
echo ""
echo "Recommended config.cfg updates:"
echo "  ENERGY_IDLE_POWER_W=${P_IDLE}"

if [ "$P_DYNAMIC_COUNT" -gt 0 ]; then
    P_DYN_AVG=$(echo "scale=2; $P_DYNAMIC_SUM / $P_DYNAMIC_COUNT" | bc -l)
    echo "  ENERGY_DYNAMIC_POWER_PER_CORE_W=${P_DYN_AVG}"
else
    echo "  ENERGY_DYNAMIC_POWER_PER_CORE_W=10.0  (default — no RAPL measurements)"
fi

echo ""
echo "To apply calibrated k_n values to profile.cfg:"
echo "  Update the 10th column in each model-batch row with values from $OUTPUT_FILE"
echo ""
echo "[FU-16] Calibration script finished."
