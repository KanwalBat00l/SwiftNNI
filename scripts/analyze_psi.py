#!/usr/bin/env python3
"""
Q-11: Post-Hoc Ψ (Psi) Metric Analyzer + Throughput/Speedup/Waiting Time Report.

Computes from the 27-column scheduler_log.csv:
  - Ψ = Σ χ_m / Σ TAT_m  (success count / total turnaround time)
  - Per-model Ψ breakdown
  - Throughput: jobs/second over the run window
  - Speedup: geometric mean of Speedup_Ratio column
  - Waiting Time: mean, P50, P95, P99 of Total_Wait_Time column
  - Tardiness: mean, P50, P95, P99 of max(0, TAT - SLO) (Contract §4)
  - Energy: sum of Energy_uJ (reported separately per Q-11)

Usage:
  python3 scripts/analyze_psi.py logs/scheduler_log.csv [--compare logs/fcfs_log.csv]
"""

import sys
import csv
import math
import argparse
from collections import defaultdict


def load_log(filepath):
    """Load semicolon-delimited CSV into list of dicts."""
    rows = []
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f, delimiter=';')
        for row in reader:
            rows.append(row)
    return rows


def compute_metrics(rows, label=""):
    """Compute Ψ, throughput, speedup, waiting time, energy metrics."""
    if not rows:
        print(f"  [{label}] No data rows found.")
        return None

    # OBSERVATION-02: Filter inference jobs only (type 'r') — exclude J_pre from Ψ.
    # Pre-processing jobs (type 'p') have empty Request_ID and Total_Wait_Time=0.
    # Inference jobs have a non-empty Request_ID and a valid Arrival_TS.
    # Belt-and-suspenders: require non-empty Request_ID AND positive Arrival_TS
    # AND non-zero Actual_TAT to exclude any internal bookkeeping rows.
    inf_rows = [r for r in rows
                if r.get('Request_ID', '').strip() != ''
                and r.get('Arrival_TS', '0').strip() not in ('0', '')
                and int(r.get('Actual_TAT', 0)) > 0]

    total_jobs = len(inf_rows)
    if total_jobs == 0:
        print(f"  [{label}] No inference jobs found.")
        return None

    # Ψ computation: Σ χ_m / Σ TAT_m
    success_count = 0
    total_tat_ms = 0
    total_wait_ms = 0
    wait_times = []
    exec_times = []
    speedup_ratios = []
    tardiness_values = []
    total_energy_uj = 0
    slo_met_count = 0

    per_model = defaultdict(lambda: {'success': 0, 'total_tat': 0, 'count': 0})

    for r in inf_rows:
        try:
            tat = int(r.get('Actual_TAT', 0))
            slo_met = int(r.get('SLO_Met', 0))
            wait = int(r.get('Total_Wait_Time', 0))
            energy = int(r.get('Energy_uJ', 0))
            model_batch = r.get('Model_Batch', 'unknown')

            total_tat_ms += tat
            total_wait_ms += wait
            wait_times.append(wait)
            total_energy_uj += energy

            if slo_met == 1:
                success_count += 1
                slo_met_count += 1

            per_model[model_batch]['total_tat'] += tat
            per_model[model_batch]['count'] += 1
            if slo_met == 1:
                per_model[model_batch]['success'] += 1

            # Contract §4: Tardiness = max(0, actual_tat - requested_slo)
            slo_req = int(r.get('Requested_SLO', 0))
            tardiness = max(0, tat - slo_req)
            # Also try the explicit Tardiness column if present
            tardiness_str = r.get('Tardiness', '')
            if tardiness_str:
                try:
                    tardiness = int(tardiness_str)
                except ValueError:
                    pass
            tardiness_values.append(tardiness)

            # Parse new columns if available
            exec_str = r.get('Exec_Time_ms', '0')
            speedup_str = r.get('Speedup_Ratio', '0')
            if exec_str:
                exec_times.append(int(exec_str))
            if speedup_str and float(speedup_str) > 0:
                speedup_ratios.append(float(speedup_str))
        except (ValueError, KeyError):
            continue

    # Ψ = Σ χ / Σ TAT
    psi = success_count / (total_tat_ms / 1000.0) if total_tat_ms > 0 else 0.0

    # Throughput: jobs / total wall time
    arrival_times = []
    finish_times = []
    for r in inf_rows:
        try:
            arrival_times.append(int(r.get('Arrival_TS', 0)))
            finish_times.append(int(r.get('Finish_TS', 0)))
        except ValueError:
            pass
    if arrival_times and finish_times:
        wall_time_s = (max(finish_times) - min(arrival_times)) / 1000.0
    else:
        wall_time_s = 0
    throughput = total_jobs / wall_time_s if wall_time_s > 0 else 0.0

    # Speedup: geometric mean
    if speedup_ratios:
        log_sum = sum(math.log(s) for s in speedup_ratios)
        geo_mean_speedup = math.exp(log_sum / len(speedup_ratios))
    else:
        geo_mean_speedup = 0.0

    # Waiting time percentiles
    wait_times.sort()
    n = len(wait_times)
    mean_wait = sum(wait_times) / n if n > 0 else 0
    p50_wait = wait_times[n // 2] if n > 0 else 0
    p95_wait = wait_times[int(n * 0.95)] if n > 0 else 0
    p99_wait = wait_times[int(n * 0.99)] if n > 0 else 0

    # Print results
    prefix = f"[{label}] " if label else ""
    print(f"\n{'='*60}")
    print(f"  {prefix}Ψ Metric Analysis Report")
    print(f"{'='*60}")
    print(f"  Total Jobs:           {total_jobs}")
    print(f"  SLO Met:              {slo_met_count}/{total_jobs} ({100*slo_met_count/total_jobs:.1f}%)")
    print(f"  Ψ (Psi):              {psi:.6f} (success/sec)")
    print(f"  Throughput:           {throughput:.3f} jobs/sec (over {wall_time_s:.1f}s)")
    if geo_mean_speedup > 0:
        print(f"  Speedup (geo mean):   {geo_mean_speedup:.4f}x")
    print(f"\n  Waiting Time:")
    print(f"    Mean:  {mean_wait:.0f} ms")
    print(f"    P50:   {p50_wait} ms")
    print(f"    P95:   {p95_wait} ms")
    print(f"    P99:   {p99_wait} ms")
    # Tardiness percentiles (Contract §4)
    tardiness_values.sort()
    nt = len(tardiness_values)
    mean_tard = sum(tardiness_values) / nt if nt > 0 else 0
    p50_tard = tardiness_values[nt // 2] if nt > 0 else 0
    p95_tard = tardiness_values[int(nt * 0.95)] if nt > 0 else 0
    p99_tard = tardiness_values[int(nt * 0.99)] if nt > 0 else 0
    nonzero_tard = sum(1 for t in tardiness_values if t > 0)
    print(f"\n  Tardiness (Contract §4):")
    print(f"    Jobs with tardiness > 0: {nonzero_tard}/{nt}")
    print(f"    Mean:  {mean_tard:.0f} ms")
    print(f"    P50:   {p50_tard} ms")
    print(f"    P95:   {p95_tard} ms")
    print(f"    P99:   {p99_tard} ms")
    print(f"\n  Energy:  {total_energy_uj} μJ total")
    print(f"\n  Per-Model Breakdown:")
    for model, stats in sorted(per_model.items()):
        m_psi = stats['success'] / (stats['total_tat'] / 1000.0) if stats['total_tat'] > 0 else 0
        m_rate = 100 * stats['success'] / stats['count'] if stats['count'] > 0 else 0
        print(f"    {model:20s}  n={stats['count']:4d}  SLO_met={m_rate:5.1f}%  Ψ={m_psi:.6f}")
    print(f"{'='*60}")

    return {
        'psi': psi,
        'throughput': throughput,
        'speedup': geo_mean_speedup,
        'mean_wait': mean_wait,
        'p95_wait': p95_wait,
        'mean_tardiness': mean_tard,
        'p95_tardiness': p95_tard,
        'slo_rate': 100 * slo_met_count / total_jobs if total_jobs > 0 else 0,
        'total_jobs': total_jobs
    }


def main():
    parser = argparse.ArgumentParser(description='Q-11: Ψ Metric Analyzer')
    parser.add_argument('logfile', help='Path to scheduler_log.csv')
    parser.add_argument('--compare', help='Optional second logfile for comparison (e.g., FCFS baseline)')
    args = parser.parse_args()

    rows = load_log(args.logfile)
    label = args.logfile.split('/')[-1].replace('.csv', '')
    m1 = compute_metrics(rows, label)

    if args.compare:
        rows2 = load_log(args.compare)
        label2 = args.compare.split('/')[-1].replace('.csv', '')
        m2 = compute_metrics(rows2, label2)

        if m1 and m2:
            print(f"\n{'='*60}")
            print(f"  Comparison: {label} vs {label2}")
            print(f"{'='*60}")
            if m2['psi'] > 0:
                psi_improvement = ((m1['psi'] - m2['psi']) / m2['psi']) * 100
                print(f"  Ψ improvement:    {psi_improvement:+.1f}% ({m1['psi']:.6f} vs {m2['psi']:.6f})")
            if m2['throughput'] > 0:
                tp_improvement = ((m1['throughput'] - m2['throughput']) / m2['throughput']) * 100
                print(f"  Throughput delta:  {tp_improvement:+.1f}%")
            if m2['mean_wait'] > 0:
                wait_improvement = ((m2['mean_wait'] - m1['mean_wait']) / m2['mean_wait']) * 100
                print(f"  Wait time delta:   {wait_improvement:+.1f}% (lower is better)")
            print(f"  SLO rate:          {m1['slo_rate']:.1f}% vs {m2['slo_rate']:.1f}%")
            print(f"{'='*60}")


if __name__ == '__main__':
    main()
