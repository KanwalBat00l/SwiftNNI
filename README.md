# SwiftNNI
> The older working implementation is there, (the max you might need to fix the path of SNNI_dir)
- Task 1: Restructure the existing code
  - Consideration make it extendable or at least manageable.
- Task 2: Define boilerplate for SLO-Scheduler.
  - Consideration make it modular so it can be extended.

### Phase 2:
# SAPPIS: SLO-Aware PPNNI Inference Scheduler

A high-performance scheduler and server manager for a bi_phase privacy preserving neural network such as *SHARK*. Optimized for multi-core HPC environments.

## Features
- **Proactive Buffering:** Pre-generates FSS keys to hide latency due to input independent pre-processing phase.
- **Resource Management:** Thread-safe allocation of cores and ports.
- **Differential Buffering:** Model-specific buffer sizes to save memory and storage.
- **SLO-Awareness:** Various algorithms for scheduling requests based on SLO requirements.
- **Hardware Telemetry:** Logs CPU, RAM, and Energy (RAPL) per job.

## Setup
1. Update `config.cfg` with your host IP and SNNI directory.
2. Update `profile.cfg` with historical model latencies.
3. Clone, shark, apply patch and then Build SHARK to support concurrent requests.

## Compilation
```bash
g++ -std=c++17 main.cpp ConfigManager.cpp ResourceManager.cpp -o sappis_server -lpthread
g++ -std=c++17 sappis_client.cpp ConfigManager.cpp -o sappis_client
```

## Usage
Start the server:
`./sappis_server`
Submit a request:
`./sappis_client <IP> <Port> <Model> <Batch_Size> <SLO_ms>`