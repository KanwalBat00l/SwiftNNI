#!/usr/bin/env python3
"""
Q-12: Poisson Test Harness for KairosSNNI Scheduler.

Generates 1000 synthetic client requests following a Poisson arrival process,
sends them to the Kairos scheduler, and collects SLO compliance statistics.

Contract specification (Q-12):
  - N = 1000 requests
  - λ ∈ {0.1, 0.5, 1.0, 2.0} requests/second (CLI argument)
  - Model mix: 70% lightweight, 20% mid-tier, 10% heavyweight
  - Randomized SLOs: D_{i,k} = E_{i,c} × Uniform(1.5, 4.0)
  - θ_c = 1.0 (symmetric harness)

Usage:
    python3 scripts/poisson_test_harness.py [options]

Options:
    --config PATH     System config file (default: config.cfg)
    --profile PATH    Model profile file (default: profile.cfg)
    --lambda RATE     Poisson arrival rate (default: 1.0)
    --requests N      Number of requests (default: 1000)
    --output PATH     Results CSV output path (default: logs/poisson_test_results.csv)
    --scheduler MODE  Override scheduler mode label for output
"""

import socket
import time
import random
import sys
import os
import csv
import math
import argparse
from dataclasses import dataclass, field
from typing import List, Dict, Tuple, Optional

# ============================================================
# Configuration Defaults
# ============================================================
DEFAULT_CONFIG = "config.cfg"
DEFAULT_PROFILE = "profile.cfg"
DEFAULT_LAMBDA = 1.0
DEFAULT_NUM_REQUESTS = 1000
CONNECT_TIMEOUT = 5.0    # TCP connect timeout (seconds)
RECV_TIMEOUT = 30.0      # TCP receive timeout (seconds)

# Q-12: Model mix — 70% lightweight, 20% mid-tier, 10% heavyweight
# Tier classification per contract:
#   Lightweight (70%): SimC1, SimC2  (small models, fast inference)
#   Mid-tier    (20%): HiNet        (medium complexity)
#   Heavyweight (10%): AlexNet, VGG16, D4, ResNet50 (large models)
MODEL_MIX_TIERS = {
    "lightweight": {
        "weight": 0.70,
        "models": ["simc1_1", "simc2_1"],
    },
    "midtier": {
        "weight": 0.20,
        "models": ["hinet_1"],
    },
    "heavyweight": {
        "weight": 0.10,
        "models": ["alexnet_1", "vgg16_1", "d4_1", "resnet50_1"],
    },
}

# Valid lambda rates per Q-12
VALID_LAMBDAS = [0.1, 0.5, 1.0, 2.0]


@dataclass
class ProfileEntry:
    model: str
    batch: int
    preproc_ms: int
    inference_ms: int
    threads: int
    max_buffer: int
    file_size_mb: int
    pre_mem_mb: int
    inf_mem_mb: int


@dataclass
class RequestResult:
    request_id: int
    model: str
    batch: int
    slo_ms: int
    send_time: float
    response: str
    accepted: bool
    tier: str = ""
    suggested_slo: int = 0


# ============================================================
# Helpers
# ============================================================
def parse_config(path: str) -> Dict[str, str]:
    """Parse key=value config file, ignoring comments and blank lines."""
    cfg = {}
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if '=' in line:
                key, val = line.split('=', 1)
                cfg[key.strip()] = val.strip()
    return cfg


def parse_profiles(path: str) -> Dict[str, ProfileEntry]:
    """Parse profile.cfg into ProfileEntry dict keyed by model_batch."""
    profiles = {}
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = [p.strip() for p in line.split(',')]
            if len(parts) >= 9:
                p = ProfileEntry(
                    model=parts[0],
                    batch=int(parts[1]),
                    preproc_ms=int(parts[2]),
                    inference_ms=int(parts[3]),
                    threads=int(parts[4]),
                    max_buffer=int(parts[5]),
                    file_size_mb=int(parts[6]),
                    pre_mem_mb=int(parts[7]),
                    inf_mem_mb=int(parts[8]),
                )
                key = f"{p.model}_{p.batch}"
                profiles[key] = p
    return profiles


def generate_poisson_arrivals(lam: float, n: int) -> List[float]:
    """Generate n inter-arrival times from Poisson process with rate lam."""
    return [random.expovariate(lam) for _ in range(n)]


def pick_model_tiered(profiles: Dict[str, ProfileEntry]) -> Tuple[str, str]:
    """
    Q-12: Weighted random selection using 70/20/10 tier mix.
    Returns (model_key, tier_name).
    """
    # Build available models per tier
    available_tiers = {}
    for tier_name, tier_info in MODEL_MIX_TIERS.items():
        valid_models = [m for m in tier_info["models"] if m in profiles]
        if valid_models:
            available_tiers[tier_name] = {
                "weight": tier_info["weight"],
                "models": valid_models,
            }

    if not available_tiers:
        # Fallback: uniform over all profiles
        key = random.choice(list(profiles.keys()))
        return key, "unknown"

    # Normalize weights for available tiers
    total_weight = sum(t["weight"] for t in available_tiers.values())
    tier_names = list(available_tiers.keys())
    tier_weights = [available_tiers[t]["weight"] / total_weight for t in tier_names]

    # Pick tier
    chosen_tier = random.choices(tier_names, weights=tier_weights, k=1)[0]

    # Pick model uniformly within tier
    chosen_model = random.choice(available_tiers[chosen_tier]["models"])
    return chosen_model, chosen_tier


def compute_randomized_slo(prof: ProfileEntry, theta_c: float = 1.0) -> int:
    """
    Q-12: Compute randomized SLO.
    D_{i,k} = E_{i,c} × Uniform(1.5, 4.0)
    where E_{i,c} = e_pre + θ_c × e_on
    """
    e_ic = prof.preproc_ms + (theta_c * prof.inference_ms)
    slo_factor = random.uniform(1.5, 4.0)
    return int(e_ic * slo_factor)


def send_request(server_ip: str, port: int, model: str, batch: int,
                 slo_ms: int) -> str:
    """Send a single request to Kairos scheduler, return admission response.

    Implements the full Kairos protocol:
      1. Send request string
      2. Q-2: Handle TTFR_PING handshake (respond with TTFR_PONG)
      3. Q-3: Handle NEGOTIATE proposal (auto-accept alternatives)
      4. Return final admission response (ACCEPTED / REJECTED_*)
    """
    msg = f"r_{model}_{batch}_{slo_ms}"
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(CONNECT_TIMEOUT)
        sock.connect((server_ip, port))
        sock.sendall(msg.encode())
        sock.settimeout(RECV_TIMEOUT)
        response = sock.recv(1024).decode().strip()

        # Q-2: TTFR Handshake — respond to PING probe
        if "TTFR_PING" in response:
            sock.sendall(b"TTFR_PONG")
            response = sock.recv(1024).decode().strip()

        # Q-3: Negotiation — auto-accept model alternatives
        if response.startswith("NEGOTIATE:"):
            sock.sendall(b"ACCEPT")
            response = sock.recv(1024).decode().strip()

        sock.close()
        return response
    except socket.timeout:
        return "TIMEOUT"
    except ConnectionRefusedError:
        return "CONNECTION_REFUSED"
    except Exception as e:
        return f"ERROR:{str(e)}"


# ============================================================
# Main
# ============================================================
def main():
    parser = argparse.ArgumentParser(
        description='Q-12: Poisson Test Harness for KairosSNNI')
    parser.add_argument('--config', default=DEFAULT_CONFIG,
                        help=f'System config file (default: {DEFAULT_CONFIG})')
    parser.add_argument('--profile', default=DEFAULT_PROFILE,
                        help=f'Model profile file (default: {DEFAULT_PROFILE})')
    parser.add_argument('--lambda', dest='lam', type=float, default=DEFAULT_LAMBDA,
                        help=f'Poisson arrival rate λ (default: {DEFAULT_LAMBDA})')
    parser.add_argument('--requests', type=int, default=DEFAULT_NUM_REQUESTS,
                        help=f'Number of requests (default: {DEFAULT_NUM_REQUESTS})')
    parser.add_argument('--output', default=None,
                        help='Results CSV output path (default: logs/poisson_<lambda>.csv)')
    parser.add_argument('--scheduler', default=None,
                        help='Scheduler mode label for output')
    parser.add_argument('--seed', type=int, default=None,
                        help='Random seed for reproducibility')
    args = parser.parse_args()

    # Validate lambda
    lam = args.lam
    if lam <= 0:
        print(f"[Error] Lambda must be > 0, got {lam}")
        sys.exit(1)

    num_requests = args.requests
    if num_requests <= 0:
        print(f"[Error] Requests must be > 0, got {num_requests}")
        sys.exit(1)

    # Set random seed if provided (for reproducibility across scheduler comparisons)
    if args.seed is not None:
        random.seed(args.seed)

    # Validate config/profile files
    if not os.path.exists(args.config):
        print(f"[Error] Config not found: {args.config}")
        sys.exit(1)
    if not os.path.exists(args.profile):
        print(f"[Error] Profile not found: {args.profile}")
        sys.exit(1)

    # Load configuration
    cfg = parse_config(args.config)
    server_ip = cfg.get("SERVER_IP", "127.0.0.1")
    scheduler_port = int(cfg.get("SCHEDULER_PORT", "8000"))
    scheduler_mode = args.scheduler or cfg.get("SCHEDULER_MODE", "UNKNOWN")

    # Load profiles
    profiles = parse_profiles(args.profile)
    print(f"[Harness] Loaded {len(profiles)} model profiles")
    print(f"[Harness] Target: {server_ip}:{scheduler_port}")
    print(f"[Harness] Scheduler mode: {scheduler_mode}")
    print(f"[Harness] Poisson lambda: {lam} req/s, total: {num_requests}")
    print(f"[Harness] Model mix: 70% lightweight, 20% mid-tier, 10% heavyweight")
    print(f"[Harness] SLO formula: D = E_{{i,c}} x Uniform(1.5, 4.0)")

    # Show available models per tier
    for tier_name, tier_info in MODEL_MIX_TIERS.items():
        available = [m for m in tier_info["models"] if m in profiles]
        missing = [m for m in tier_info["models"] if m not in profiles]
        pct = int(tier_info["weight"] * 100)
        print(f"[Harness]   {tier_name} ({pct}%): available={available}"
              + (f" missing={missing}" if missing else ""))
    print()

    # Generate arrival schedule
    inter_arrivals = generate_poisson_arrivals(lam, num_requests)
    total_time = sum(inter_arrivals)
    print(f"[Harness] Scheduled {num_requests} requests over ~{total_time:.1f}s")
    print(f"[Harness] Starting test...\n")

    results: List[RequestResult] = []
    accepted_count = 0
    rejected_count = 0
    rejected_403_count = 0  # OBSERVATION-01: Tier 1/2 rejections (Practicality/Asymmetry)
    rejected_404_count = 0  # OBSERVATION-01: Tier 3 rejections (VFT/Congestion)
    error_count = 0
    tier_stats: Dict[str, Dict[str, int]] = {
        "lightweight": {"total": 0, "accepted": 0},
        "midtier": {"total": 0, "accepted": 0},
        "heavyweight": {"total": 0, "accepted": 0},
    }
    suggested_slo_values: List[int] = []  # OBSERVATION-01: Collect suggested SLOs for Wait-Time Delta analysis

    for i in range(num_requests):
        # Wait for next arrival
        if i > 0:
            time.sleep(inter_arrivals[i])

        # Q-12: Pick model using 70/20/10 tier mix
        model_key, tier = pick_model_tiered(profiles)
        prof = profiles[model_key]

        # Q-12: Randomized SLO with θ_c = 1.0 (symmetric harness)
        slo_ms = compute_randomized_slo(prof, theta_c=1.0)

        # Send request
        send_time = time.time()
        response = send_request(server_ip, scheduler_port, prof.model, prof.batch, slo_ms)

        # Parse result
        accepted = response == "ACCEPTED"
        suggested_slo = 0
        if "SUGGESTED_SLO:" in response:
            try:
                suggested_slo = int(response.split(":")[-1])
            except ValueError:
                pass

        result = RequestResult(
            request_id=i + 1,
            model=prof.model,
            batch=prof.batch,
            slo_ms=slo_ms,
            send_time=send_time,
            response=response,
            accepted=accepted,
            tier=tier,
            suggested_slo=suggested_slo,
        )
        results.append(result)

        # Update counters — OBSERVATION-01: Differentiate REJECTED_403 vs REJECTED_404
        if accepted:
            accepted_count += 1
            status_str = "ACCEPTED"
        elif "REJECTED_404" in response:
            rejected_count += 1
            rejected_404_count += 1
            if suggested_slo:
                suggested_slo_values.append(suggested_slo)
            status_str = f"REJECTED_404 (suggested: {suggested_slo}ms)" if suggested_slo else "REJECTED_404"
        elif "REJECTED_403" in response:
            rejected_count += 1
            rejected_403_count += 1
            status_str = f"REJECTED_403 ({response.split(':',1)[1]})" if ':' in response else "REJECTED_403"
        elif "REJECTED" in response:
            rejected_count += 1
            status_str = f"REJECTED (suggested: {suggested_slo}ms)" if suggested_slo else "REJECTED"
        else:
            error_count += 1
            status_str = response

        # Update tier stats
        if tier in tier_stats:
            tier_stats[tier]["total"] += 1
            if accepted:
                tier_stats[tier]["accepted"] += 1

        # Progress output (every 50 requests or first/last)
        if (i + 1) % 50 == 0 or i == 0 or i == num_requests - 1:
            elapsed = time.time() - results[0].send_time if results else 0
            print(f"  [{i+1:4d}/{num_requests}] {prof.model}_{prof.batch} "
                  f"SLO={slo_ms:5d}ms [{tier:12s}] -> {status_str}  "
                  f"(elapsed: {elapsed:.1f}s)")

    # ============================================================
    # Statistics
    # ============================================================
    elapsed_total = results[-1].send_time - results[0].send_time if len(results) > 1 else 0
    effective_lambda = (num_requests - 1) / elapsed_total if elapsed_total > 0 else 0

    print(f"\n{'='*70}")
    print(f"  Q-12 POISSON TEST HARNESS RESULTS")
    print(f"{'='*70}")
    print(f"  Scheduler:       {scheduler_mode}")
    print(f"  Lambda (target):  {lam} req/s")
    print(f"  Lambda (actual):  {effective_lambda:.3f} req/s")
    print(f"  Total requests:   {num_requests}")
    print(f"  Elapsed time:     {elapsed_total:.1f}s")
    print(f"  Accepted:         {accepted_count} ({100*accepted_count/num_requests:.1f}%)")
    print(f"  Rejected (total): {rejected_count} ({100*rejected_count/num_requests:.1f}%)")
    print(f"    REJECTED_403:   {rejected_403_count} (Tier 1/2: Practicality/Static SLO)")
    print(f"    REJECTED_404:   {rejected_404_count} (Tier 3: VFT Congestion)")
    print(f"  Errors:           {error_count} ({100*error_count/num_requests:.1f}%)")
    # OBSERVATION-01: Wait-Time Delta analysis from SUGGESTED_SLO values
    if suggested_slo_values:
        slo_delta_vals = sorted(suggested_slo_values)
        nd = len(slo_delta_vals)
        print(f"\n  Wait-Time Delta (SUGGESTED_SLO from REJECTED_404):")
        print(f"    Samples:  {nd}")
        print(f"    Min:      {slo_delta_vals[0]} ms")
        print(f"    Median:   {slo_delta_vals[nd // 2]} ms")
        print(f"    Max:      {slo_delta_vals[-1]} ms")
        print(f"    Mean:     {sum(slo_delta_vals) / nd:.0f} ms")
    print()

    # Tier breakdown (Q-12 verification)
    print(f"  Tier Breakdown (target: 70/20/10):")
    print(f"  {'Tier':<15s} {'Total':>6s} {'Accept':>7s} {'Reject':>7s} {'Mix%':>6s} {'AccRate':>8s}")
    print(f"  {'-'*49}")
    for tier_name in ["lightweight", "midtier", "heavyweight"]:
        s = tier_stats[tier_name]
        mix_pct = 100 * s["total"] / num_requests if num_requests > 0 else 0
        acc_rate = 100 * s["accepted"] / s["total"] if s["total"] > 0 else 0
        rej = s["total"] - s["accepted"]
        print(f"  {tier_name:<15s} {s['total']:>6d} {s['accepted']:>7d} {rej:>7d} {mix_pct:>5.1f}% {acc_rate:>7.1f}%")
    print()

    # Per-model breakdown
    model_stats: Dict[str, Dict[str, int]] = {}
    for r in results:
        key = f"{r.model}_{r.batch}"
        if key not in model_stats:
            model_stats[key] = {"total": 0, "accepted": 0, "rejected": 0}
        model_stats[key]["total"] += 1
        if r.accepted:
            model_stats[key]["accepted"] += 1
        else:
            model_stats[key]["rejected"] += 1

    print(f"  Per-Model Breakdown:")
    print(f"  {'Model':<15s} {'Total':>6s} {'Accept':>7s} {'Reject':>7s} {'Rate':>7s}")
    print(f"  {'-'*42}")
    for key in sorted(model_stats.keys()):
        s = model_stats[key]
        rate = 100 * s["accepted"] / s["total"] if s["total"] > 0 else 0
        print(f"  {key:<15s} {s['total']:>6d} {s['accepted']:>7d} {s['rejected']:>7d} {rate:>6.1f}%")

    # SLO distribution statistics
    slo_values = [r.slo_ms for r in results]
    slo_values.sort()
    n = len(slo_values)
    if n > 0:
        print(f"\n  SLO Distribution (ms):")
        print(f"    Min:   {slo_values[0]}")
        print(f"    P25:   {slo_values[n // 4]}")
        print(f"    P50:   {slo_values[n // 2]}")
        print(f"    P75:   {slo_values[int(n * 0.75)]}")
        print(f"    Max:   {slo_values[-1]}")
        print(f"    Mean:  {sum(slo_values) / n:.0f}")

    # Save results CSV
    output_path = args.output or f"logs/poisson_lambda{lam}_{scheduler_mode}.csv"
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else "logs", exist_ok=True)
    with open(output_path, 'w', newline='') as f:
        writer = csv.writer(f, delimiter=';')
        writer.writerow(["RequestID", "Model", "Batch", "SLO_ms", "Tier",
                         "SendTime", "Response", "Accepted", "SuggestedSLO",
                         "RejectionType"])
        for r in results:
            # OBSERVATION-01: Classify rejection type for downstream analysis
            rej_type = ""
            if not r.accepted:
                if "REJECTED_404" in r.response:
                    rej_type = "404"
                elif "REJECTED_403" in r.response:
                    rej_type = "403"
                elif "REJECTED" in r.response:
                    rej_type = "UNKNOWN"
            writer.writerow([r.request_id, r.model, r.batch, r.slo_ms, r.tier,
                             f"{r.send_time:.3f}", r.response,
                             1 if r.accepted else 0, r.suggested_slo,
                             rej_type])

    print(f"\n  Results saved to: {output_path}")

    # Also write a summary line for tournament aggregation
    summary_path = output_path.replace(".csv", "_summary.txt")
    with open(summary_path, 'w') as f:
        f.write(f"scheduler={scheduler_mode}\n")
        f.write(f"lambda={lam}\n")
        f.write(f"requests={num_requests}\n")
        f.write(f"accepted={accepted_count}\n")
        f.write(f"rejected={rejected_count}\n")
        f.write(f"rejected_403={rejected_403_count}\n")
        f.write(f"rejected_404={rejected_404_count}\n")
        f.write(f"errors={error_count}\n")
        f.write(f"acceptance_rate={100*accepted_count/num_requests:.1f}\n")
        f.write(f"elapsed_s={elapsed_total:.1f}\n")
        if suggested_slo_values:
            f.write(f"suggested_slo_mean={sum(suggested_slo_values)/len(suggested_slo_values):.0f}\n")
            f.write(f"suggested_slo_median={sorted(suggested_slo_values)[len(suggested_slo_values)//2]}\n")

    print(f"  Summary saved to: {summary_path}")
    print(f"{'='*70}")

    # Return exit code based on whether test completed successfully
    return 0 if error_count < num_requests * 0.5 else 1


if __name__ == "__main__":
    sys.exit(main())
