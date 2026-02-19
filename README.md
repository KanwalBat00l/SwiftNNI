# Kairos: Autonomous SLO-Aware Orchestrator for Secure Inference

Kairos is a high-performance middleware orchestration server designed for **Privacy-Preserving Neural Network Inference (PPNNI)** protocols, specifically optimized for the *SHARK* protocol. It transforms "one-shot" cryptographic benchmarks into multi-tenant, SLO-aware services capable of running on high-core-count HPC environments like Snellius (AMD Genoa).

## Key Features

*   **Proactive Resource Provisioning:** A background `Replenisher` engine manages a proactive buffer of cryptographic material (Dealer phase) to hide high-latency pre-processing times.
*   **Multi-Tenant Execution:** Utilizing dynamic file-prefixing to allow multiple concurrent inferences of the same model-batch pair without file-system collisions.
*   **Elastic Resource Management:** Programmatic sensing of node RAM and CPU affinity. Supports **Elastic Under-allocation** to prevent job starvation during high-density bursts.
*   **7 Scheduling Algorithms** through a unified resource-aware bypass engine:
    *   **Heuristics:** FCFS (with backfilling), SJF-Aging, EDF, LST
    *   **Optimization:** Simulated Annealing (SA) with 4D perturbation, Deep Q-Network (DQN) with hot-reload
    *   **Custom:** SPLS (State-Prioritized Least-Slack)
*   **3-Tier Admission Control:** Tier 1 (hard minimum), Tier 2 (static SLO filter), Tier 3 (VFT dynamic feasibility)
*   **Client Negotiation:** TTFR handshake probe, per-network-type theta baselines, n_alt model alternatives with family-based mapping
*   **Hardware Telemetry:** Real-time asynchronous logging of CPU load, RAM utilization, and hybrid Energy consumption (RAPL + Slurm `sacct` + FU-15 Linear Power Model)
*   **DQN Training Pipeline:** Multi-objective reward (SLO + TAT + Energy), imitation learning bootstrap on EDF traces, DQN2 binary export with CRC32 integrity, FU-12 minimum sample guard
*   **Per-Model Parallelism (FU-14):** Model-specific Amdahl's Law exponent k_n replaces global k=0.8

---

## Project Structure

The codebase follows a modular C++17 architecture:

*   `include/` — Header files: `IScheduler` interface, `ResourceManager`, scheduler implementations (FCFS, SJF, EDF, LST, SA, DQN, Custom), `Types.hpp`
*   `src/` — Core engine (`KairosServer.cpp`), config parsing, logging, telemetry, test suite
*   `scripts/` — Python training pipeline (`train_dqn.py`), Poisson harness, tournament runner, Psi analyzer
*   `logs/` — CSV performance logs, dynamic profiles, memory traces

---

## Setup & Configuration

### 1. Requirements
*   A compiled version of the **SHARK** binary (with the dynamic prefix patch applied).
*   Linux environment with `taskset` support (for core pinning).
*   C++17 compatible compiler (GCC 9+).
*   Python 3.8+ with `torch`, `pandas`, `numpy` (for DQN training only).

### 2. Configuration Files
*   **`config.cfg`** — System configuration: `SERVER_IP`, `TOTAL_CORES=24`, `SCHEDULER_MODE`, SA/DQN parameters, TTFR baselines, negotiation thresholds, memory safety limits
*   **`profile.cfg`** — Baseline execution times, memory requirements, and per-model Amdahl exponent (k_n) per model-batch pair
*   **`client.cfg`** — Command template for the SHARK client-side binary

### Key Configuration Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `TOTAL_CORES` | 24 | Physical cores allocated (FU-1) |
| `SCHEDULER_MODE` | FCFS | Active scheduler: FCFS/SJF/EDF/LST/SA/DQN/CUSTOM |
| `VFT_SAFETY_MARGIN` | 1.2 | VFT admission control margin |
| `SA_INITIAL_TEMP` | 100.0 | SA starting temperature |
| `SA_COOLING_RATE` | 0.92 | SA geometric cooling |
| `SA_WINDOW_SIZE` | 100 | SA full-queue optimization depth |
| `MEM_SAFETY_THRESHOLD` | 0.90 | Memory pressure threshold |
| `THETA_NEGOTIATION_THRESHOLD` | 1.5 | Theta trigger for n_alt negotiation |
| `DQN_REWARD_W1_SLO` | 10.0 | Multi-objective DQN SLO weight (FU-8) |
| `N_ALT_MAP` | (see config) | Family-based model alternative mapping (FU-6) |
| `ENERGY_IDLE_POWER_W` | 100.0 | FU-15: Linear power model idle power (W) |
| `ENERGY_DYNAMIC_POWER_PER_CORE_W` | 10.0 | FU-15: Per-core dynamic power (W) |

---

## Quick Start (Toy Deployment)

See [`Howto.md`](Howto.md) for Slurm allocation commands and edge server specs.

```bash
# 1. Compile Server and Client
make all

# 2. Run with toy settings (8-core, 16GB simulation)
./Kairos_server -c toy_config.cfg -p toy_profile.cfg &

# 3. Execute the 10-request workload script
chmod +x toy_run.sh
./toy_run.sh
```

### Edge Node (24-core production)
```bash
./Kairos_server -c config.cfg -p profile.cfg &
```

---

## Compilation

```bash
# Compile both Server and Client
make all

# Clean build artifacts
make clean

# Build and run test suite
make test
```

---

## Implementation Status

All contract items (C-01~C-32, Q-1~Q-13) and follow-up answers (FU-1~FU-16, ClientAnswers6 BUG-01/OBSERVATION-01/OBSERVATION-02) are implemented.

### Contract Items Summary

| Category | Items | Status |
|----------|-------|--------|
| Core Architecture | C-01 ~ C-07 | All DONE |
| Scheduling Algorithms | C-08 ~ C-20 | All DONE |
| Resource Management | C-21 ~ C-28 | All DONE |
| Monitoring & Logging | C-29 ~ C-32 | All DONE |
| Follow-Up Questions | Q-1 ~ Q-13 | All DONE |
| Client Answers 4 | FU-1 ~ FU-10 | All DONE |
| Client Answers 5 | FU-11 ~ FU-16 | All DONE |
| Client Answers 6 | BUG-01, OBSERVATION-01, OBSERVATION-02 | All DONE |

### FU-1 ~ FU-10 (ClientAnswers4.pdf)

| Item | Description | Implementation |
|------|-------------|---------------|
| FU-1 | Target Core Count = 24 | `TOTAL_CORES=24`, SA bounds `[1, min(16, totalCores)]`, DQN normalization |
| FU-2 | DQN Imitation Learning | Bootstrap on EDF traces, 500-1000 episodes (`train_dqn.py`) |
| FU-3 | Amdahl's Law Speedup | `T(p) = e_pre + (theta_c * e_on) / p^k_n` in SA cost function (FU-14: per-model k_n) |
| FU-4 | Centralized Triple Check | `popReadyJob()`: File + Core (query) + Memory (SystemMonitor) |
| FU-5 | Dual-Phase Welford | Separate `welfordUpdateOn()`/`welfordUpdatePre()` trackers |
| FU-6 | Family-Based N_ALT_MAP | `alexnet:simc2,hinet` format; batch preserved from original request |
| FU-7 | Lightweight Auto-Penalty | Bottom-of-chain models get `phi=1`, skip negotiation |
| FU-8 | Multi-Objective DQN Reward | `R = w1*R_SLO + w2*R_TAT + w3*R_Energy`, configurable weights |
| FU-9 | Per-Network TTFR Baselines | LAN=5ms, WiFi=20ms, 5G=40ms, WAN=100ms |
| FU-10 | Phase 1 Complete | 38 tests, 272/272 checks passed |

### FU-11 ~ FU-16 (ClientAnswers5.pdf)

| Item | Description | Implementation |
|------|-------------|---------------|
| FU-11 | Tournament Hardware = 24 cores | `config.cfg:TOTAL_CORES=24`, lambda=[0.1,0.5,1.0,2.0] |
| FU-12 | MIN_TRAIN_SAMPLES = 50000 | `train_dqn.py`: guard check + temporal split + alignment accuracy |
| FU-13 | Phase Roadmap (all in Phase 1) | Informational — all 5 logical phases delivered in Phase 1 |
| FU-14 | Per-Model Amdahl k_n | `Types.hpp:parallel_efficiency_k`, profile.cfg 10th column, SA uses per-model k |
| FU-15 | Linear Power Model Fallback | `SystemMonitor::estimateEnergyLinearModel()`, P_idle=100W, P_dyn=10W/core |
| FU-16 | Hybrid Calibration Script | `scripts/calibrate_hardware.sh` — offline k_n fitting + RAPL idle measurement |

### ClientAnswers6 (BUG-01, OBSERVATION-01, OBSERVATION-02)

| Item | Description | Implementation |
|------|-------------|---------------|
| BUG-01 | Tournament Script Fix | `run_kairos_tournament.sh` line 140: positional args → `-c`/`-p` flags |
| OBSERVATION-01 | Differentiated Rejection Parsing | `poisson_test_harness.py`: REJECTED_403/404 counters, RejectionType CSV column, Wait-Time Delta analysis |
| OBSERVATION-02 | Preprocessing Arrival_TS & Psi Exclusion | (a) `KairosServer.cpp:549` sets `arrival_ts = start_ts`, (b) `analyze_psi.py` belt-and-suspenders filter |

---

## Test Suite

**Version:** v7.1 | **Tests:** 38 | **Checks:** 272/272 PASSED

```bash
make test
```

| Tests | Coverage |
|-------|----------|
| 1-4 | Heuristic schedulers: FCFS, SJF, EDF, LST |
| 5-7 | SA: basic operation, SLO-aware reordering, memory pressure |
| 8-11 | DQN: LST fallback, DQN2 header, raw weights, legacy/bad file handling |
| 12-14 | Config parsing, SchedulerFactory, CUSTOM/SPLS |
| 15 | FU-5: Dual-phase Welford (pre + on) |
| 16-18 | Q-1 Cap_c, Q-6 SA malleable cores, Q-7 DQN hot-reload |
| 19-22 | Q-2 TTFR, Q-4 Tier 2, Q-3 Negotiation, Config v2 |
| 23-25 | C-22 Dynamic watchdog, Q-11 Psi metric, C-17 SA retrigger |
| 26-29 | Contract Section 4 (CT/Tardiness), Q-12 Poisson, Q-5 metrics |
| 30 | FU-3: Amdahl's Law speedup model verification |
| 31 | FU-5: Dual-phase Welford independence |
| 32 | FU-9: Per-network TTFR baseline configuration |
| 33 | Logger: 27-column CSV output verification (scheduler_log.csv) |
| 34 | FU-14: Per-model Amdahl's Law exponent k_n |
| 35 | FU-15: Linear Power Model fallback energy estimation |
| 36 | FU-14: Profile.cfg k_n column parsing (backward compatible) |
| 37 | OBSERVATION-02a: Preprocessing Job Arrival_TS = Start_TS |
| 38 | OBSERVATION-02b: Psi Metric Excludes J_pre Rows |

---

## Scheduling Tournament

Run the full tournament comparing all 7 schedulers across Poisson arrival rates:

```bash
# Tournament: 7 modes x 4 lambda rates
bash scripts/run_kairos_tournament.sh

# Individual Poisson test
python3 scripts/poisson_test_harness.py --lambda 1.0 --requests 1000

# Post-hoc analysis
python3 scripts/analyze_psi.py logs/SA_scheduler_log.csv --compare logs/FCFS_scheduler_log.csv
```

### Psi Utility Metric

The primary evaluation metric is **Psi** (success density per unit latency):

```
Psi = sum(chi_m) / sum(TAT_m)
```

Where `chi_m = 1` if job met its SLO, `TAT_m = Finish_TS - Arrival_TS`.

---

## DQN Training

```bash
# Train DQN model on accumulated scheduling logs
python3 scripts/train_dqn.py

# Output: models/dqn_weights.bin (DQN2 format with CRC32)
```

The training pipeline:
1. **FU-12 Sample Guard**: Requires MIN_TRAIN_SAMPLES=50000 before training proceeds
2. **FU-2 Imitation Learning**: If no existing model, bootstrap on EDF traces (500-1000 epochs)
3. **FU-8 Multi-Objective Reward**: `R = 10.0*R_SLO + 1.0*R_TAT + 0.1*R_Energy`
4. **FU-12 Temporal Validation**: 80/20 chronological split (most recent 20% for test) + 90% alignment accuracy target
5. **Export**: DQN2 binary (28-byte header + float32 weights) for C++ hot-reload

---

## Performance Logging

The Logger produces a **27-column semicolon-delimited CSV** (`scheduler_log.csv`) with:

- Timestamps: Arrival, Start, Finish, Completion_Time
- SLO: Requested_SLO, SLO_Met, SLO_Diff, Tardiness
- Resources: CPU_Load, Mem_GB, Energy_uJ, Assigned_Cores
- Client: theta_est, theta_act, penalty_phi, Original_Model
- Metrics: Exec_Time, Speedup_Ratio, Throughput_Local, Actual_TAT

---

## Verification Summary (2026-02-18)

Full source code analysis and build verification completed against `Developer_COntract_Updated.pdf` (C-01~C-32, Q-1~Q-13), `ClientAnswers4.pdf` (FU-1~FU-10), `ClientAnswers5.pdf` (FU-11~FU-16), and `ClientAnswers6.pdf` (BUG-01, OBSERVATION-01, OBSERVATION-02).

| Check | Result |
|-------|--------|
| SHARK build at `../shark` | Verified (all benchmark binaries present) |
| `make all` (Server + Client) | Compiled with 0 warnings (C++17, -Wall -Wextra -O3) |
| `make test` (272 checks) | **272/272 PASSED** across 38 test categories |
| TOTAL_CORES=24 (FU-1) | `config.cfg:16`, SA bounds `[1,16]`, DQN normCores=24 |
| Per-Model Amdahl k_n (FU-14) | `SAScheduler.hpp:186`, `profile.cfg` 10th column, verified in Tests 34/36 |
| Centralized Triple Check (FU-4) | `IScheduler.hpp:33-80` popReadyJob() with File+Core+Memory |
| Dual-Phase Welford (FU-5) | `Types.hpp:42-118`, verified in Tests 15 and 31 |
| Family-Based n_alt (FU-6) | `KairosServer.cpp:110-136`, batch preserved |
| Auto-Penalty Lightweight (FU-7) | `KairosServer.cpp:339-343`, phi=1 when no n_alt |
| Multi-Objective DQN (FU-8) | `config.cfg:88-91`, w1=10/w2=1/w3=0.1 |
| Per-Network TTFR (FU-9) | `KairosServer.cpp:279-292`, verified in Test 32 |
| 27-Column CSV Logger | `Logger.cpp`, verified in Test 33 (full field-by-field check) |
| MIN_TRAIN_SAMPLES (FU-12) | `train_dqn.py`: 50000 sample guard + temporal split |
| Linear Power Model (FU-15) | `SystemMonitor.cpp`, verified in Test 35 |
| Calibration Script (FU-16) | `scripts/calibrate_hardware.sh`, k_n fitting + RAPL idle |
| BUG-01 Fix (ClientAnswers6) | `run_kairos_tournament.sh` line 140: `-c`/`-p` flags |
| OBSERVATION-01 (ClientAnswers6) | `poisson_test_harness.py`: REJECTED_403/404 differentiation |
| OBSERVATION-02 (ClientAnswers6) | `KairosServer.cpp:549` + `analyze_psi.py` J_pre exclusion, verified in Tests 37-38 |

---

## Algorithms (Pseudocode)

The following pseudocode describes the core algorithms implemented in Kairos, formatted in the style of the SLO-Aware Scheduling reference paper.

### Algorithm 1: Three-Tier Admission Control

```
Algorithm 1: Three-Tier Admission Control
─────────────────────────────────────────────────────────────
Input:  Job request J = (model, batch, D), SystemConfig C,
        ModelProfiles P, SystemSnapshot S
Output: ACCEPT | REJECT | NEGOTIATE

 1: procedure ADMIT(J, C, P, S)
 2:   key ← J.model + "_" + J.batch
 3:   prof ← P[key]
 4:
 5:   // ── Tier 1: Hard Minimum Filter ──
 6:   if J.D < prof.inference_ms then
 7:     return REJECT    ▷ SLO shorter than raw inference time
 8:   end if
 9:
10:   // ── Tier 2: Static SLO Filter (Q-4) ──
11:   θ_c ← max(1.0, J.theta_est)
12:   E_ic ← prof.e_pre + θ_c × prof.e_on
13:   if J.D < C.slo_factor_tier2 × E_ic then
14:     return REJECT    ▷ SLO too tight for estimated execution
15:   end if
16:
17:   // ── Tier 3: VFT Dynamic Feasibility ──
18:   vft_finish ← estimateVFT(J, queue, S)
19:   if vft_finish > J.D × C.vft_safety_margin then
20:     // Attempt negotiation (Q-3)
21:     if C.n_alt_map contains J.model then
22:       alternatives ← C.n_alt_map[J.model]
23:       for alt in alternatives do
24:         J' ← J with model ← alt  ▷ FU-6: preserve batch
25:         if ADMIT(J', C, P, S) = ACCEPT then
26:           return NEGOTIATE(alt)
27:         end if
28:       end for
29:     end if
30:     return REJECT
31:   end if
32:
33:   return ACCEPT
34: end procedure
```

### Algorithm 2: SA Priority Mapping with 4D Perturbation

```
Algorithm 2: Simulated-Annealing Priority Mapping (C-12/C-16/Q-6)
─────────────────────────────────────────────────────────────
Input:  Queue Q of pending jobs, ModelProfiles P,
        total_mem_gb M, SA parameters (T₀, α, I_max, W)
Output: Optimized job ordering in Q

 1: procedure SA_SORT(Q, P, M, T₀, α, I_max, W)
 2:   if |Q| < 2 or ¬needsOptimization then return
 3:   if now - lastOptimizeTs < minRetriggerMs then return  ▷ C-17
 4:
 5:   seq ← Q[0 : min(|Q|, W)]     ▷ C-18: Full-queue window
 6:   T ← T₀
 7:   cost_curr ← COST(seq, P, M)
 8:   cost_best ← cost_curr
 9:   seq_best ← seq
10:
11:   for i = 1 to I_max while T > 1.0 do
12:     ptype ← RANDOM(0, 3)         ▷ 4D perturbation
13:     seq' ← PERTURB(seq, ptype, P)
14:     cost_new ← COST(seq', P, M)
15:     Δ ← cost_new − cost_curr
16:
17:     if Δ < 0 or exp(−Δ/T) > RAND(0,1) then
18:       seq ← seq'                 ▷ Accept move
19:       cost_curr ← cost_new
20:       if cost_curr < cost_best then
21:         cost_best ← cost_curr
22:         seq_best ← seq
23:       end if
24:     end if
25:     T ← T × α                    ▷ Geometric cooling
26:   end for
27:
28:   Q[0 : |seq_best|] ← seq_best
29: end procedure
30:
31: function PERTURB(seq, ptype, P)
32:   case ptype of
33:     0: swap_jobs(seq, i, j)           ▷ Dim 1: Random swap
34:     1: shift_dwell_time(seq, i → j)   ▷ Dim 2: Position shift
35:     2: reverse_subseq(seq, i..j)      ▷ Dim 3: Reverse segment
36:     3: adjust_cores(seq[i], [1..16])   ▷ Dim 4: Q-6 Malleable
37: end function
38:
39: function COST(seq, P, M)             ▷ C-16 Multi-objective
40:   cost ← 0; timeline ← now
41:   for each job J in seq do
42:     prof ← P[J.key]
43:     p_J ← J.scheduled_cores or prof.threads
44:     k_n ← prof.parallel_efficiency_k   ▷ FU-14
45:     T_J ← prof.e_pre + (θ_c × prof.e_on) / p_J^k_n  ▷ FU-3
46:     timeline ← timeline + T_J
47:     tardiness ← max(0, timeline − deadline_J)
48:     cost ← cost + tardiness²            ▷ Term 1
49:           + 10 × max(0, mem − 0.9M)     ▷ Term 2
50:           + slack × 0.01                 ▷ Term 3
51:           + 100 × max(0, Σp − P)        ▷ Term 4: Q-6
52:           + 500 × φ_J                   ▷ Term 5: Q-3
53:   end for
54:   return cost
55: end function
```

### Algorithm 3: Centralized Triple-Check Dispatch (FU-4)

```
Algorithm 3: Centralized Triple-Check Dispatch
─────────────────────────────────────────────────────────────
Input:  Sorted queue Q, FileManager F, ResourceManager R,
        SystemMonitor SM, SystemConfig C
Output: Next dispatchable job J* or ∅

 1: procedure POP_READY_JOB(Q, F, R, SM, C)
 2:   snap ← SM.takeSnapshot()
 3:
 4:   for each job J in Q (priority order) do
 5:     key ← J.model + "_" + J.batch
 6:
 7:     // ── Check 1: File Availability ──
 8:     file ← F.acquireFile(key)
 9:     if file = ∅ then continue           ▷ No pre-processed data
10:
11:     // ── Check 2: Core Availability ──
12:     p_J ← J.scheduled_cores or P[key].threads
13:     cores ← R.acquireCores(p_J)
14:     if cores = ∅ then
15:       F.releaseFile(file)
16:       continue                           ▷ Insufficient cores
17:     end if
18:
19:     // ── Check 3: Memory Safety (C-23) ──
20:     if snap.mem_used_gb / snap.total_mem_gb > C.mem_safety_threshold then
21:       R.releaseCores(cores)
22:       F.releaseFile(file)
23:       continue                           ▷ Memory pressure
24:     end if
25:
26:     // All checks passed — dispatch
27:     J.assigned_file ← file
28:     J.assigned_cores ← cores
29:     remove J from Q
30:     return J
31:   end for
32:
33:   return ∅                               ▷ No job dispatchable
34: end procedure
```

### Algorithm 4: TTFR Handshake and Client Negotiation (Q-2/Q-3)

```
Algorithm 4: TTFR Handshake and Client Negotiation
─────────────────────────────────────────────────────────────
Input:  Client socket S, Job request J, SystemConfig C
Output: Updated J with θ_est, possibly altered model

 1: procedure TTFR_HANDSHAKE(S, J, C)
 2:   // ── Timer 1: TCP RTT Measurement ──
 3:   t₁ ← now()
 4:   send(S, "TTFR_PING")
 5:   recv(S, "TTFR_PONG")
 6:   t₂ ← now()
 7:   rtt_ms ← t₂ − t₁
 8:
 9:   // ── FU-9: Per-Network-Type Baseline ──
10:   baseline ← C.ttfr_baseline[J.net_type]  ▷ LAN=5, WiFi=20, 5G=40, WAN=100
11:   J.theta_est ← max(1.0, rtt_ms / baseline)
12:
13:   // ── Q-3: Negotiation Check ──
14:   if J.theta_est ≥ C.theta_negotiation_threshold then
15:     if C.n_alt_map contains J.model then
16:       for alt in C.n_alt_map[J.model] do
17:         alt_key ← alt + "_" + J.batch     ▷ FU-6: batch preserved
18:         if alt_key in P then
19:           send(S, "NEGOTIATE:" + alt_key)
20:           resp ← recv(S, timeout=C.negotiation_timeout_ms)
21:           if resp = "ACCEPT" then
22:             J.original_model ← J.model
23:             J.model ← alt
24:             return J
25:           else
26:             J.penalty_phi ← 1             ▷ Q-3: refusal penalty
27:           end if
28:         end if
29:       end for
30:     end if
31:     // FU-7: Bottom-of-chain → phi=1, skip negotiation
32:   end if
33:
34:   return J
35: end procedure
```

### Algorithm 5: DQN Pointwise Ranking with Hot-Reload (C-19/Q-7)

```
Algorithm 5: DQN Pointwise Ranking
─────────────────────────────────────────────────────────────
Input:  Queue Q, DQN model W (5→64→32→1), SystemSnapshot S
Output: Sorted queue Q by predicted priority score

 1: procedure DQN_SORT(Q, W, S, P)
 2:   if W = ∅ then
 3:     return LST_SORT(Q)                   ▷ Fallback to LST
 4:   end if
 5:
 6:   scores ← []
 7:   for each job J in Q do
 8:     key ← J.model + "_" + J.batch
 9:     prof ← P[key]
10:     slack ← (J.D − (now − J.arrival_ts) − prof.e_on) / 1000
11:     x ← [slack, S.cpu_load/100, S.mem_used/S.total_mem,
12:           prof.threads/24, prof.inf_mem/total_mem]
13:     score ← W.forward(x)                ▷ 5→64→32→1 MLP
14:     scores.append((J, score))
15:   end for
16:
17:   sort Q by scores descending
18: end procedure
19:
20: // Q-7: Hot-Reload Thread
21: procedure DQN_WATCHER(path, interval=5s)
22:   mtime_prev ← stat(path).mtime
23:   loop
24:     sleep(interval)
25:     mtime_curr ← stat(path).mtime
26:     if mtime_curr > mtime_prev then
27:       W' ← LOAD_DQN2(path)              ▷ DQN2 header + CRC32
28:       if W' ≠ ∅ then W ← W'
29:       mtime_prev ← mtime_curr
30:     end if
31:   end loop
32: end procedure
```

### Algorithm 6: Hybrid Energy Audit (FU-15)

```
Algorithm 6: Hybrid Energy Audit with Linear Power Model Fallback
─────────────────────────────────────────────────────────────
Input:  Active cores p_J, TAT t_ms, Config C
Output: Energy in microjoules

 1: function ENERGY_AUDIT(p_J, t_ms, C)
 2:   // ── Priority 1: Hardware RAPL ──
 3:   E ← readRAPL("/sys/class/powercap/intel-rapl:0/energy_uj")
 4:   if E > 0 then return E
 5:
 6:   // ── Priority 2: Slurm sacct ──
 7:   E ← parseSlurmEnergy(SLURM_JOB_ID)
 8:   if E > 0 then return E
 9:
10:   // ── Priority 3: FU-15 Linear Power Model ──
11:   P_total ← C.P_idle + p_J × C.P_dynamic_per_core
12:   E ← P_total × (t_ms / 1000) × 10⁶     ▷ W × s → μJ
13:   return E
14: end function
```

---

## Documentation

| Document | Description |
|----------|-------------|
| `Howto.md` | Slurm allocation commands, build/run guide, edge server specs |
| `ClientAnswers4.pdf` | Client answers to FU-1~FU-10 |
| `ClientAnswers5.pdf` | Client answers to FU-11~FU-16 |
| `ClientAnswers6.pdf` | Client answers to BUG-01, OBSERVATION-01, OBSERVATION-02 |
