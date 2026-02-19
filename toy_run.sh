#!/bin/bash
# Refined Scenario Runner
# NOTE: SRV must match SERVER_IP in the active config file (toy_config.cfg or config.cfg)
SRV="127.0.0.1"; PRT="8000"

# --- ZONE 1: HoL Bypassing & Concurrency ---
./Kairos_client $SRV $PRT alexnet 1 120000 & # R1: Giant (Takes ~20s), generous SLO
sleep 0.1
./Kairos_client $SRV $PRT simc1 1 30000 &    # R2: Swift (Ready, bypasses R1)
./Kairos_client $SRV $PRT hinet 1 30000 &    # R3: Concurrent with R2

# --- ZONE 2: SJF vs. LST vs. EDF (The Clash) ---
# We send 3 requests while cores are busy with R1, R2, R3
sleep 0.5
./Kairos_client $SRV $PRT hinet 1 40000 &    # R4: Long (5s), Far SLO (SJF hates, EDF likes)
./Kairos_client $SRV $PRT simc1 1 30000 &    # R5: Short (3s), Moderate SLO
./Kairos_client $SRV $PRT simc2 1 40000 &    # R6: Medium (8s), Adequate SLO

# --- ZONE 3: Admission & Buffer Exhaustion ---
sleep 2.0
./Kairos_client $SRV $PRT hinet 1 5500 &     # R7: Strict SLO (Should Reject — Tier 2)
./Kairos_client $SRV $PRT simc1 1 30000 &    # R8: Moderate SLO
./Kairos_client $SRV $PRT simc1 1 30000 &    # R9: Moderate SLO
./Kairos_client $SRV $PRT simc1 1 30000 &    # R10: Buffer Exhausted check

wait
