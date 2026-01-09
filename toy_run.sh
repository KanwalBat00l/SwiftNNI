#!/bin/bash
# Refined Scenario Runner
SRV="172.18.59.203"; PRT="8000"

# --- ZONE 1: HoL Bypassing & Concurrency ---
./sappis_client $SRV $PRT alexnet 1 100000 & # R1: Giant (Takes 20s)
sleep 0.1
./sappis_client $SRV $PRT simc1 1 10000 &    # R2: Swift (Ready, bypasses R1)
./sappis_client $SRV $PRT hinet 1 15000 &    # R3: Concurrent with R2

# --- ZONE 2: SJF vs. LST vs. EDF (The Clash) ---
# We send 3 requests while cores are busy with R1, R2, R3
sleep 0.5
./sappis_client $SRV $PRT hinet 1 40000 &    # R4: Long (5s), Far SLO (SJF hates, EDF likes)
./sappis_client $SRV $PRT simc1 1 8000 &     # R5: Short (3s), Tight SLO (LST/SJF loves)
./sappis_client $SRV $PRT simc2 1 12000 &    # R6: Medium (8s), Zero Slack (LST Priority)

# --- ZONE 3: Admission & Buffer Exhaustion ---
sleep 2.0
./sappis_client $SRV $PRT hinet 1 5500 &     # R7: Strict SLO (Should Reject)
./sappis_client $SRV $PRT simc1 1 10000 &    # R8: Ready file?
./sappis_client $SRV $PRT simc1 1 10000 &    # R9: Ready file?
./sappis_client $SRV $PRT simc1 1 10000 &    # R10: Buffer Exhausted check

wait