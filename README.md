# SwiftNNI: High-Performance Neural Network Inference Scheduler

SwiftNNI is a lightweight, low-latency scheduling engine designed for optimized Neural Network Inference (NNI) workloads. It manages the lifecycle of model execution—from proactive pre-processing to dynamic resource allocation—ensuring high throughput and fair execution across heterogeneous request types.

## Key Features

*   **Priority-Aware Scheduling**: Supports both **FCFS** (First-Come-First-Served) and **SJF** (Shortest Job First) with an integrated **Aging Mechanism** to prevent long-task starvation.
*   **Proactive Pre-processing**: A dedicated replenishment loop ensures that model-specific pre-processed files are generated in advance, hiding compilation latency from the end-user.
*   **Single-Use File Lifecycle**: Implements a secure "Just-in-Time" file management system where unique, timestamped model files are generated, used once for inference, and immediately purged to ensure system consistency.
*   **Dynamic Watchdog**: Employs factor-based process monitoring that scales timeouts based on static performance profiles, protecting the system from hung processes without being overly restrictive.
*   **Resource Throttling**: Intelligent thread and port management prevents CPU over-subscription and network collisions during high-concurrency scenarios.

## System Architecture

SwiftNNI is built on a modular "Mechanism vs. Policy" architecture:

1.  **Policy Layer**: The `IScheduler` interface and its implementations (SJF, FCFS) define the logic for job selection.
2.  **Infrastructure Layer**: Specialized Managers handle Resource counting (`ResourceManager`), File state-tracking (`FileManager`), and Static Profiling (`ConfigManager`).
3.  **Execution Layer**: The `ProcessRunner` handles low-level process spawning, directory isolation, and environment variable injection for Shark/IREE backends.
4.  **Orchestration**: The `SwiftServer` coordinates the concurrent Listener, Dispatcher, and Replenisher loops.

## Getting Started

### Prerequisites
*   C++17 compatible compiler (e.g., GCC 7+)
*   Linux Environment (uses `fork`, `exec`, and POSIX sockets)
*   Thread support (`pthread`)

### Building
To build the server and the client application:
```bash
make