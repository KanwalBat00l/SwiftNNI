#!/bin/bash
# =============================================================================
# Q-13: Kairos Scheduler Tournament Script
#
# Runs the Poisson benchmark harness against all 7 scheduler modes and
# produces a comparison report using analyze_psi.py.
#
# Contract spec (Q-13):
#   - Phase 1: FCFS, SJF, EDF, LST, SA, DQN, CUSTOM (7 modes)
#   - Poisson harness with λ = {0.1, 0.5, 1.0, 2.0}
#   - Target: 20% Ψ improvement over FCFS baseline
#   - Output: Comparison table per lambda rate
#
# Usage:
#   ./scripts/run_kairos_tournament.sh [config] [profile] [lambda]
#
# Arguments:
#   config   Path to system config (default: config.cfg)
#   profile  Path to profile.cfg (default: profile.cfg)
#   lambda   Specific lambda rate (default: run all 4 rates)
#
# Prerequisites:
#   - Kairos_server must be built (make)
#   - profile.cfg must exist with model entries
#   - Python3 available with scripts/poisson_test_harness.py
#   - scripts/analyze_psi.py available for post-hoc analysis
# =============================================================================

set -euo pipefail

# Configuration
CONFIG="${1:-config.cfg}"
PROFILE="${2:-profile.cfg}"
LAMBDA_SINGLE="${3:-}"
SEED=42  # Fixed seed for reproducible comparison

# All scheduler modes to test (Q-13 Phase 1)
SCHEDULER_MODES=("FCFS" "SJF" "EDF" "LST" "SA" "DQN" "CUSTOM")

# Lambda rates per Q-12
if [ -n "$LAMBDA_SINGLE" ]; then
    LAMBDA_RATES=("$LAMBDA_SINGLE")
else
    LAMBDA_RATES=("0.1" "0.5" "1.0" "2.0")
fi

# Directories
LOG_DIR="logs/tournament"
RESULTS_DIR="$LOG_DIR/results"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors (if terminal supports it)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================
# Functions
# ============================================================
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_ok() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_prerequisites() {
    log_info "Checking prerequisites..."

    if [ ! -f "$CONFIG" ]; then
        log_error "Config file not found: $CONFIG"
        exit 1
    fi

    if [ ! -f "$PROFILE" ]; then
        log_error "Profile file not found: $PROFILE"
        exit 1
    fi

    if [ ! -f "$SCRIPT_DIR/poisson_test_harness.py" ]; then
        log_error "Poisson harness not found: $SCRIPT_DIR/poisson_test_harness.py"
        exit 1
    fi

    if [ ! -f "$SCRIPT_DIR/analyze_psi.py" ]; then
        log_error "Psi analyzer not found: $SCRIPT_DIR/analyze_psi.py"
        exit 1
    fi

    if ! command -v python3 &> /dev/null; then
        log_error "python3 not found in PATH"
        exit 1
    fi

    if [ ! -f "Kairos_server" ]; then
        log_warn "Kairos_server binary not found. Building..."
        make -j$(nproc) || {
            log_error "Build failed"
            exit 1
        }
    fi

    log_ok "All prerequisites met"
}

# Start Kairos server with a specific scheduler mode
start_server() {
    local mode="$1"
    local server_pid=""

    log_info "Starting Kairos_server with SCHEDULER_MODE=$mode..."

    # Create temporary config with overridden scheduler mode
    local tmp_config="$LOG_DIR/config_${mode}.cfg"
    sed "s/^SCHEDULER_MODE=.*/SCHEDULER_MODE=${mode}/" "$CONFIG" > "$tmp_config"

    # Ensure log directory exists
    mkdir -p logs

    # Clean previous scheduler log to avoid accumulation between runs
    local log_path
    log_path=$(grep "^LOG_FILE=" "$CONFIG" | cut -d= -f2)
    if [ -n "$log_path" ] && [ -f "$log_path" ]; then
        rm -f "$log_path"
    fi

    # Start server in background (redirect output to avoid contaminating PID capture)
    ./Kairos_server -c "$tmp_config" -p "$PROFILE" > "$LOG_DIR/server_${mode}.log" 2>&1 &
    server_pid=$!

    # Wait for server listener to be ready (check port)
    local sched_port
    sched_port=$(grep "^SCHEDULER_PORT=" "$CONFIG" | cut -d= -f2)
    sched_port="${sched_port:-8000}"
    local max_wait=15
    for i in $(seq 1 "$max_wait"); do
        sleep 2
        if ss -tlnp 2>/dev/null | grep -q ":${sched_port} "; then
            log_ok "Server listening on port $sched_port (mode=$mode)"
            break
        fi
        if ! kill -0 "$server_pid" 2>/dev/null; then
            log_error "Server died during startup for mode $mode"
            return 1
        fi
    done

    # Verify server is running
    if ! kill -0 "$server_pid" 2>/dev/null; then
        log_error "Server failed to start for mode $mode"
        return 1
    fi

    echo "$server_pid"
}

# Stop Kairos server and cleanup child processes
stop_server() {
    local pid="$1"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        log_info "Stopping server (PID: $pid)..."
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    # Kill any leftover SHARK benchmark processes spawned by the server
    pkill -f "benchmark-" 2>/dev/null || true
    sleep 2
    # Ensure the scheduler port is free before next round
    local sched_port
    sched_port=$(grep "^SCHEDULER_PORT=" "$CONFIG" | cut -d= -f2)
    sched_port="${sched_port:-8000}"
    local retries=5
    while ss -tlnp 2>/dev/null | grep -q ":${sched_port} " && [ "$retries" -gt 0 ]; do
        sleep 1
        retries=$((retries - 1))
    done
}

# Run Poisson harness for a specific scheduler mode and lambda
run_benchmark() {
    local mode="$1"
    local lam="$2"
    local output_csv="$RESULTS_DIR/${mode}_lambda${lam}.csv"

    log_info "  Running benchmark: mode=$mode lambda=$lam..."

    python3 "$SCRIPT_DIR/poisson_test_harness.py" \
        --config "$CONFIG" \
        --profile "$PROFILE" \
        --lambda "$lam" \
        --requests 1000 \
        --scheduler "$mode" \
        --seed "$SEED" \
        --output "$output_csv" \
        2>&1 | tail -30

    return ${PIPESTATUS[0]}
}

# Analyze scheduler log with Psi metric
analyze_log() {
    local log_file="$1"
    local label="$2"

    if [ -f "$log_file" ]; then
        python3 "$SCRIPT_DIR/analyze_psi.py" "$log_file" 2>&1
    else
        log_warn "Log file not found: $log_file"
    fi
}

# Generate comparison report
generate_report() {
    local lam="$1"
    local report_file="$LOG_DIR/tournament_report_lambda${lam}.txt"

    echo "============================================================" > "$report_file"
    echo "  Q-13 KAIROS SCHEDULER TOURNAMENT REPORT"                    >> "$report_file"
    echo "  Lambda = $lam req/s | $(date)"                              >> "$report_file"
    echo "============================================================" >> "$report_file"
    echo ""                                                              >> "$report_file"

    # Collect FCFS baseline Psi for comparison
    local fcfs_log="logs/scheduler_log_FCFS.csv"
    local fcfs_psi=""

    echo "  Mode       | Accepted | Rejected | Errors | Acc Rate" >> "$report_file"
    echo "  -----------|----------|----------|--------|----------" >> "$report_file"

    for mode in "${SCHEDULER_MODES[@]}"; do
        local summary="$RESULTS_DIR/${mode}_lambda${lam}_summary.txt"
        if [ -f "$summary" ]; then
            local accepted=$(grep "^accepted=" "$summary" | cut -d= -f2)
            local rejected=$(grep "^rejected=" "$summary" | cut -d= -f2)
            local errors=$(grep "^errors=" "$summary" | cut -d= -f2)
            local acc_rate=$(grep "^acceptance_rate=" "$summary" | cut -d= -f2)
            printf "  %-10s | %8s | %8s | %6s | %6s%%\n" \
                "$mode" "$accepted" "$rejected" "$errors" "$acc_rate" >> "$report_file"
        else
            printf "  %-10s | %8s | %8s | %6s | %6s\n" \
                "$mode" "N/A" "N/A" "N/A" "N/A" >> "$report_file"
        fi
    done

    echo "" >> "$report_file"
    echo "  Note: For full Psi metric analysis, run:" >> "$report_file"
    echo "    python3 scripts/analyze_psi.py logs/scheduler_log.csv" >> "$report_file"
    echo "    python3 scripts/analyze_psi.py logs/scheduler_log.csv --compare logs/fcfs_baseline.csv" >> "$report_file"
    echo "" >> "$report_file"
    echo "  Q-13 Target: 20% Psi improvement over FCFS baseline" >> "$report_file"
    echo "============================================================" >> "$report_file"

    log_ok "Report saved: $report_file"
    cat "$report_file"
}

# ============================================================
# Main Tournament Logic
# ============================================================
main() {
    echo "============================================================"
    echo "  Q-13: Kairos Scheduler Tournament"
    echo "  Modes: ${SCHEDULER_MODES[*]}"
    echo "  Lambda rates: ${LAMBDA_RATES[*]}"
    echo "  Config: $CONFIG | Profile: $PROFILE"
    echo "============================================================"
    echo ""

    check_prerequisites

    # Create output directories
    mkdir -p "$LOG_DIR" "$RESULTS_DIR"

    # Record tournament start
    local start_time=$(date +%s)

    for lam in "${LAMBDA_RATES[@]}"; do
        echo ""
        echo "============================================================"
        echo "  LAMBDA = $lam req/s"
        echo "============================================================"

        for mode in "${SCHEDULER_MODES[@]}"; do
            echo ""
            echo "------------------------------------------------------------"
            echo "  Testing: $mode @ lambda=$lam"
            echo "------------------------------------------------------------"

            # Start server
            local server_pid
            server_pid=$(start_server "$mode") || {
                log_error "Failed to start server for $mode, skipping"
                continue
            }

            # Run benchmark
            run_benchmark "$mode" "$lam" || {
                log_warn "Benchmark returned non-zero for $mode @ lambda=$lam"
            }

            # Copy scheduler log for this run
            if [ -f "logs/scheduler_log.csv" ]; then
                cp "logs/scheduler_log.csv" "$RESULTS_DIR/scheduler_log_${mode}_lambda${lam}.csv"

                # Analyze with Psi metric
                analyze_log "$RESULTS_DIR/scheduler_log_${mode}_lambda${lam}.csv" "${mode}_${lam}"
            fi

            # Stop server
            stop_server "$server_pid"

            # Clean up for next run
            sleep 1
        done

        # Generate per-lambda comparison report
        generate_report "$lam"
    done

    # Record tournament end
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    echo ""
    echo "============================================================"
    echo "  TOURNAMENT COMPLETE"
    echo "  Duration: ${duration}s"
    echo "  Results:  $RESULTS_DIR/"
    echo "  Reports:  $LOG_DIR/tournament_report_*.txt"
    echo ""
    echo "  For Psi comparison between any two modes:"
    echo "    python3 scripts/analyze_psi.py \\"
    echo "      $RESULTS_DIR/scheduler_log_SA_lambda1.0.csv \\"
    echo "      --compare $RESULTS_DIR/scheduler_log_FCFS_lambda1.0.csv"
    echo "============================================================"
}

# ============================================================
# Entry Point
# ============================================================
main "$@"
