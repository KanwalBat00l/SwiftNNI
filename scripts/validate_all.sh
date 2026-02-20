#!/bin/bash
# =============================================================================
# SwiftNNI Full Validation Suite
# Runs every model/batch combination from the profile exactly once.
# =============================================================================

SERVER_IP="172.18.60.90"
SERVER_PORT="8000"

# All 18 combinations from your profile.cfg
MODELS=(
    "simc1_1" "simc1_2" "simc1_4" "simc1_8" "simc1_16" "simc1_32"
    "hinet_1" "hinet_2" "hinet_4" "hinet_8" "hinet_16"
    "simc2_1" "simc2_2" "simc2_4" "simc2_8"
    "alexnet_1" "alexnet_2" "alexnet_4"
)

echo "--- Starting Full SwiftNNI Validation (18 Jobs) ---"
echo "Target Server: $SERVER_IP:$SERVER_PORT"

for M_B in "${MODELS[@]}"; do
    MODEL=$(echo $M_B | cut -d'_' -f1)
    BATCH=$(echo $M_B | cut -d'_' -f2)
    
    echo -n "[Testing] $MODEL Batch $BATCH ... "
    
    # Run the client and capture output
    ./Swift_client $SERVER_IP $SERVER_PORT $MODEL $BATCH > /dev/null 2>&1
    
    if [ $? -eq 0 ]; then
        echo "SUCCESS"
    else
        echo "FAILED (Segmentation Fault or Timeout)"
    fi
    
    # Small pause to let the server cleanup the file and threads
    sleep 1
done

echo "----------------------------------------------------"
echo "Validation Complete. Check scheduler_log.csv for RCs."