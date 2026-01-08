#!/bin/bash
# SAPPIS Toy Scenario Runner
SRV="127.0.0.1"
PRT="8000"

echo "[Toy Demo] T=0ms: Sending AlexNet (Giant, No Buffer)..."
./sappis_client $SRV $PRT alexnet 1 100000 &
sleep 0.1

echo "[Toy Demo] T=100ms: Sending Simc1 (Small, Ready)..."
./sappis_client $SRV $PRT simc1 1 5000 &
sleep 0.1

echo "[Toy Demo] T=200ms: Sending Hinet (Ready, Concurrent)..."
./sappis_client $SRV $PRT hinet 1 8000 &
sleep 0.1

echo "[Toy Demo] T=300ms: Sending Simc2 (Ready, No Cores)..."
./sappis_client $SRV $PRT simc2 1 10000 &
sleep 0.1

echo "[Toy Demo] T=400ms: Sending Hinet (Tight SLO -> Reject)..."
./sappis_client $SRV $PRT hinet 1 5500 &
sleep 0.1

echo "[Toy Demo] T=500ms: Sending Simc1 (No Buffer)..."
./sappis_client $SRV $PRT simc1 1 6000 &

echo "[Toy Demo] Waiting for queue to clear..."
sleep 10
echo "[Toy Demo] T=10s: Sending Simc2..."
./sappis_client $SRV $PRT simc2 1 15000 &
sleep 5
echo "[Toy Demo] T=15s: Sending AlexNet..."
./sappis_client $SRV $PRT alexnet 1 50000 &

wait
echo "[Toy Demo] Sequence Complete. Analyze scheduler_log.csv"