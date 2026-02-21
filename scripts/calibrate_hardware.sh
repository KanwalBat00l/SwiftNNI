#!/bin/bash
# =============================================================================
# SwiftNNI: Scientific Calibration with Failure Timing and Port Probing
# =============================================================================

SNNI_DIR="../shark"
INPUT_PROFILE="profile.cfg"
UPDATED_PROFILE="profile_updated.cfg"
AUDIT_LOG="logs/calibration_audit.csv"
REPEATS=5
START_PORT=51000

# Increase OS limits for high-frequency socket opening
ulimit -s unlimited
ulimit -n 65535

ABS_SWIFT=$(pwd)
ABS_SHARK=$(realpath "$SNNI_DIR")

mkdir -p logs
echo "model,batch,pre_ms,inf_ms,threads" > "$ABS_SWIFT/$UPDATED_PROFILE"
# Added RC column to audit
echo "model,batch,run_id,pre_ms,inf_ms,status,rc" > "$ABS_SWIFT/$AUDIT_LOG"

tail -n +2 "$ABS_SWIFT/$INPUT_PROFILE" | sed 's/\r//g' | while IFS=',' read -r model batch pre inf threads; do
    model=$(echo $model | xargs); batch=$(echo $batch | xargs); threads=$(echo $threads | xargs)
    if [ -z "$model" ]; then continue; fi

    echo ">> Calibration: $model | Batch $batch"
    SUM_PRE=0; SUM_INF=0; SUCCESS_COUNT=0
    CURRENT_PORT=$((START_PORT + RANDOM % 1000))

    for ((i=1; i<=REPEATS; i++)); do
        FILE="c_${model:0:3}_b${batch}_r${i}"
        ((CURRENT_PORT++))

        # --- PHASE 2: PRE-PROCESSING ---
        cd "$ABS_SHARK"
        T_START=$(date +%s%3N)
        ./build/benchmark-$model 2 127.0.0.1 $CURRENT_PORT $batch $FILE > /dev/null 2>&1
        RC_PRE=$?
        T_END=$(date +%s%3N)
        D_PRE=$((T_END - T_START))

        if [ $RC_PRE -ne 0 ]; then
            echo "  [Run $i] Pre-proc FAILED (RC $RC_PRE)"
            echo "$model,$batch,$i,$D_PRE,0,failed_pre,$RC_PRE" >> "$ABS_SWIFT/$AUDIT_LOG"
            continue
        fi

        # --- PHASE 0 & 1: INFERENCE ---
        export OMP_NUM_THREADS=$threads
        ./build/benchmark-$model 0 127.0.0.1 $CURRENT_PORT $batch $FILE > /dev/null 2>&1 &
        SRV_PID=$!
        
        # PROBE: Wait until the port is actually open (Max 10 seconds)
        # This replaces the unreliable fixed sleep
        PORT_READY=false
        for attempt in {1..20}; do
            if ss -ltn | grep -q ":$CURRENT_PORT "; then
                PORT_READY=true
                break
            fi
            sleep 0.5
        done

        T_START=$(date +%s%3N)
        if [ "$PORT_READY" = true ]; then
            ./build/benchmark-$model 1 127.0.0.1 $CURRENT_PORT $batch $FILE > /dev/null 2>&1
            RC_INF=$?
        else
            RC_INF=-100 # Timeout waiting for port
        fi
        T_END=$(date +%s%3N)
        D_INF=$((T_END - T_START))

        # Cleanup
        kill -9 $SRV_PID 2>/dev/null || true
        wait $SRV_PID 2>/dev/null || true
        rm "${FILE}"*.dat 2>/dev/null || true

        # LOG THE DATA REGARDLESS OF FAILURE
        if [ $RC_INF -eq 0 ]; then
            STATUS="success"
            SUM_PRE=$((SUM_PRE + D_PRE))
            SUM_INF=$((SUM_INF + D_INF))
            ((SUCCESS_COUNT++))
            echo "  [Run $i] SUCCESS: ${D_INF}ms"
        else
            STATUS="failed_inf"
            echo "  [Run $i] FAILED: RC $RC_INF at ${D_INF}ms"
        fi
        
        echo "$model,$batch,$i,$D_PRE,$D_INF,$STATUS,$RC_INF" >> "$ABS_SWIFT/$AUDIT_LOG"
        cd "$ABS_SWIFT"
    done

    # Output to updated profile only if at least one run succeeded
    if [ $SUCCESS_COUNT -gt 0 ]; then
        AVG_PRE=$((SUM_PRE / SUCCESS_COUNT))
        AVG_INF=$((SUM_INF / SUCCESS_COUNT))
        echo "$model,$batch,$AVG_PRE,$AVG_INF,$threads" >> "$ABS_SWIFT/$UPDATED_PROFILE"
    else
        # If all runs failed, put a placeholder so the scheduler doesn't crash 
        # (use 99999 to represent an "unstable" job)
        echo "$model,$batch,$D_PRE,99999,$threads" >> "$ABS_SWIFT/$UPDATED_PROFILE"
    fi
done