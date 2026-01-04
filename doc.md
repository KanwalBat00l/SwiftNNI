# SAPPIS: Project Documentation

## 1. Project Overview & Problem Statement

**SAPPIS** (*SLO-Aware PPNNI Inference Scheduler*) is a high-performance middleware designed for the **SHARK protocol**, an actively secure Neural Network Inference protocol.

### The Problem

* **One-Shot Protocol**
  SHARK was originally designed for a single execution using hardcoded filenames (`server.dat`), preventing multi-tenancy.

* **Preprocessing Latency**
  The *Dealer* phase takes ~75% of execution time. Without a proactive buffer, clients experience massive delays.

* **Resource Contention**
  On HPC clusters like **Snellius (AMD Genoa)**, multiple concurrent inferences can fight for CPU cores, causing jitter and SLO violations.

* **Static vs. Reality**
  Fixed execution times in configuration files rarely match real-world performance under system load.

### The Solution

* **Middleware Orchestration**
  A multi-threaded server that manages unique timestamped file prefixes for concurrent execution.

* **Proactive Buffering**
  A *Replenisher* thread that maintains a *Ready* pool of pre-processed files.

* **Resource Bucketing**
  Physical cores are partitioned into dedicated pools:

  * System (SAPPIS)
  * Dealer (Pre-processing)
  * Inference (SHARK Online)

* **Dynamic Profiling**
  An **Exponential Moving Average (EMA)** feedback loop updates expected execution times in real time to refine Admission Control.

---

## 2. Technical Component Registry

### Core Engine

| File                | Class           | Purpose                                     | Key Methods                                         |
| ------------------- | --------------- | ------------------------------------------- | --------------------------------------------------- |
| `Types.hpp`         | Structs         | Central data contracts                      | `ModelProfile (Atomic)`, `Job`, `SystemConfig`      |
| `SAPPISServer.cpp`  | `SAPPISServer`  | Orchestrator of background threads          | `listenerLoop`, `dispatcherLoop`, `replenisherLoop` |
| `ConfigManager.cpp` | `ConfigManager` | Robust config parser with `\r\n` sanitation | `loadSystemConfig`, `loadProfiles`                  |

---

### Resource & Data Management

| File                  | Class             | Purpose                                       | Key Methods                                  |
| --------------------- | ----------------- | --------------------------------------------- | -------------------------------------------- |
| `ResourceManager.cpp` | `ResourceManager` | Core/Port manager; handles Slurm affinity     | `acquireInferenceCores`, `acquireDealerCore` |
| `FileManager.cpp`     | `FileManager`     | State machine for pre-processed files         | `initiateFile`, `setReady`, `acquireFile`    |
| `SJFScheduler.cpp`    | `SJFScheduler`    | Shortest Job First queue with wait-time aging | `push`, `pop` (Best Score logic)             |

---

### Utilities & Telemetry

| File                  | Class             | Purpose                                | Key Methods                            |
| --------------------- | ----------------- | -------------------------------------- | -------------------------------------- |
| `SystemMonitor.cpp`   | `SystemMonitor`   | Hybrid telemetry (CPU/RAM/Energy)      | `takeSnapshot`, `getEnergyFromSlurm`   |
| `ProcessLauncher.cpp` | `ProcessLauncher` | Fork/Exec wrapper with safety timeouts | `runShellCommand`                      |
| `StringUtils.cpp`     | `StringUtils`     | Shell command template builder         | `buildCommand` (conditional `taskset`) |
| `Logger.cpp`          | `Logger`          | CSV performance recorder               | `logJob` (records 15 metrics)          |

---

## 3. How to Run the System

### Prerequisites

* `config.cfg`, `profile.cfg`, and `client.cfg` must be in the project root
* SHARK binaries must be compiled in `SNNI_DIR` (e.g., `../shark/build/`)

---

### Step 1: Start the SAPPIS Server

The server manages the node’s resources and prepares the file buffer.

```bash
./sappis_server
```

The server will detect your Slurm cores (e.g., `0–23`) and begin running *Dealers* in the background.

---

### Step 2: Run a SAPPIS Client

Run this in a separate terminal or node.
The client negotiates with the server and then executes the local SHARK client.

```bash
# Usage: ./sappis_client <Server_IP> <Port> <Model> <Batch> <SLO_ms>
./sappis_client 127.0.0.1 8000 alexnet 1 50000
```

---

### Step 3: Analyze Results

Inspect `scheduler_log.csv` for detailed performance metrics, including:

* **Wait_Time** — Time spent in the scheduler queue
* **SLO_Diff** — Slack (`+` = met SLO, `-` = violated)
* **Energy_uJ** — Energy consumed (from RAPL or Slurm `sacct`)

---

## 4. Key Logic: Dynamic Admission Control

The server maintains a moving average of execution time per model-batch:

1. A job finishes in **3500 ms**

2. SAPPIS updates the profile:

   ```
   NewAvg = (0.2 × 3500) + (0.8 × OldAvg)
   ```

3. A new client requests the same model with an SLO of **3000 ms**

4. The Listener checks:

   ```
   3000 ≥ (NewAvg × K_Factor)
   ```

5. If false, the server immediately returns:

   ```
   REJECTED_SLO_UNFEASIBLE
   ```

This avoids wasted computation and preserves system stability.

