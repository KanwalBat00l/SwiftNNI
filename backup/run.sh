#!/bin/bash

SERVER_IP="172.18.58.109"
PORT=8000
LAMBDA=0.034 # 2 Jobs per 1 min

# Convert lambda (jobs/sec) into exponential sleep
exp_rand() {
  awk -v l="$LAMBDA" -v seed="$(date +%s%N)${RANDOM}$$" '
    BEGIN {
      srand(seed);
      u = rand(); while (u<=1e-12) u = rand();
      print -log(u)/l;
    }'
}

# realistic 36
# batches=(hinet_1 alexnet_2 simc2_1 hinet_4  hinet_8 simc2_2 hinet_2 alexnet_1 simc2_8 simc2_4 hinet_2 simc2_1 alexnet_4 hinet_1 alexnet_1 hinet_1
# alexnet_8 hinet_1 hinet_2 simc2_4  hinet_8 simc2_1 hinet_2 simc2_2 alexnet_1 simc2_8 hinet_4 alexnet_8
# hinet_1 simc2_2 hinet_2 hinet_4 alexnet_2  alexnet_1 hinet_2 hinet_1)


# Balanced 62
batches=(simc2_1 hinet_1 alexnet_1 hinet_8 alexnet_8 alexnet_2 simc2_4 simc2_8 hinet_8 alexnet_4 hinet_2 simc2_1 simc2_2 hinet_1 simc2_1 hinet_1 hinet_4 alexnet_8
alexnet_2 simc2_8 hinet_8 alexnet_1 alexnet_4 hinet_4 simc2_4 alexnet_8 hinet_2 simc2_1 hinet_4 alexnet_2 simc2_2 hinet_1 hinet_2 alexnet_8
alexnet_1 simc2_4 alexnet_4 alexnet_8 simc2_1 hinet_4 simc2_2 hinet_2 alexnet_2 simc2_8 hinet_1 hinet_8
hinet_4 alexnet_4 simc2_1 alexnet_2 hinet_2 simc2_4 hinet_1 alexnet_1 simc2_2
hinet_2 simc2_2 hinet_1 alexnet_2 alexnet_1 alexnet_8 simc2_1)

REPEAT=1

# bookkeeping
started=0
completed=0
tmpfile=$(mktemp)

trap "rm -f $tmpfile; echo; echo Started: $started, Completed: $completed" EXIT

launch_job() {
    b=$1
    (
        ./client "$SERVER_IP" "$PORT" r "${b}" && \
        echo done >> "$tmpfile"
    ) &
    started=$((started+1))
    completed=$(wc -l < "$tmpfile")
    echo "Launched job $started (batch=$b). Completed so far: $completed"
}

# --- Burst phase: First 10 jobs instantly ---
job_counter=0
# --- Controlled phase: Repeat the 30 jobs N times ---
for r in $(seq 1 $REPEAT); do
    for b in "${batches[@]}"; do
        launch_job "$b"
        job_counter=$((job_counter + 1))

        # Apply sleep only after the first 2 jobs
        if [ "$job_counter" -gt 0 ]; then
            sleep_time=$(exp_rand)
            sleep "$sleep_time"
        fi
    done
    # update LAMBDA (example: double each time)
    LAMBDA=$(awk -v l="$LAMBDA" 'BEGIN { print l * 2 }')
    sleep_time=$(exp_rand)
    sleep "$sleep_time"
done

wait
completed=$(wc -l < "$tmpfile")
echo "All jobs launched. Started: $started, Completed: $completed"



#If LAMBDA=1 → average 1 job/sec.
#If LAMBDA=0.5 → average 1 job every 2 sec.
#If LAMBDA=0.1 → average 1 job every 10 sec.
#Desired avg interval	λ (per minute)	λ (per second)
#p1m 0.017  p2m 0.0084 p3m 0.0056  p4m 0.0042 p5m 0.0034
#2jpm 0.034 3jpm 0.51 4jpm 0.078  6jpm .10  8jpm .136 

