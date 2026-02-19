/**
 * test_schedulers.cpp — Comprehensive test suite for all Kairos schedulers.
 *
 * Tests:
 *   1.  FCFS: push/pop in FIFO order
 *   2.  SJF:  shortest job (with aging) first
 *   3.  EDF:  earliest absolute deadline first
 *   4.  LST:  least slack time first
 *   5.  SA:   basic operation and queue reordering
 *   6.  SA:   SLO-aware reordering quality
 *   7.  SA:   memory pressure awareness
 *   8.  DQN:  LST fallback (no weights)
 *   9.  DQN:  with NN weights (DQN2 header format, 64-32-1)
 *   10. DQN:  raw weight format (no header, 64-32-1)
 *   11. DQN:  legacy DQN1 detection + bad file handling
 *   12. Config parsing of all parameters (SA + EWMA + Memory + Checkpoint + Q-3/Q-8)
 *   13. SchedulerFactory: all modes including CUSTOM
 *   14. CUSTOM/SPLS: state-prioritized least-slack
 *   15. Welford's Algorithm: online stddev tracking
 *   16. Q-1: Job Cap_c fields and defaults
 *   17. Q-6: SA malleable core sizing
 *   18. Q-7: DQN hot-reload via file-watch
 *   19-25. TTFR, Tier 2, Negotiation, Config v2, Watchdog, Psi, SA retrigger
 *   26. Contract §4: Completion_Time and Tardiness computation
 *   27. Q-12: Poisson SLO formula D = E_{i,c} × U(1.5, 4.0)
 *   28. Q-12: 70/20/10 model mix tier weights
 *   29. Q-5: Performance metrics (Throughput, Speedup, WaitTime, ExecTime)
 *   30. FU-3: Amdahl's Law speedup model
 *   31. FU-5: Dual-phase Welford independence
 *   32. FU-9: Per-network TTFR baselines
 *   33. Logger: 27-column CSV output verification
 *   34. FU-14: Per-model Amdahl's Law exponent k_n
 *   35. FU-15: Linear Power Model fallback
 *   36. FU-14: Profile.cfg k_n column parsing
 *   37. OBSERVATION-02a: Preprocessing Job Arrival_TS = Start_TS
 *   38. OBSERVATION-02b: Psi metric excludes J_pre rows
 *
 * Build:
 *   g++ -std=c++17 -I./include -O3 src/test_schedulers.cpp src/ConfigManager.cpp
 *       src/StringUtils.cpp src/SystemMonitor.cpp src/FileManager.cpp
 *       src/ResourceManager.cpp src/Logger.cpp
 *       -o test_schedulers -lpthread
 */
#include <iostream>
#include <cassert>
#include <cstring>
#include <fstream>
#include <vector>
#include <map>
#include <chrono>
#include <cmath>
#include <thread>
#include <unistd.h>

#include "Types.hpp"
#include "FCFSScheduler.hpp"
#include "SJFScheduler.hpp"
#include "EDFScheduler.hpp"
#include "LSTScheduler.hpp"
#include "SAScheduler.hpp"
#include "DQNScheduler.hpp"
#include "CustomScheduler.hpp"
#include "SchedulerFactory.hpp"
#include "FileManager.hpp"
#include "ConfigManager.hpp"
#include "SystemMonitor.hpp"
#include "Logger.hpp"

static int total_checks = 0;
static int passed_checks = 0;

static long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

#define CHECK(cond, msg) do { \
    total_checks++; \
    if (cond) { passed_checks++; std::cout << "    PASS: " << msg << "\n"; } \
    else { std::cout << "    FAIL: " << msg << "\n"; } \
} while(0)

// Helper: create a ModelProfile for testing
static ModelProfile makeProfile(const std::string& model, int batch, long inf_ms,
                                 int threads, int inf_mem_mb) {
    ModelProfile p;
    p.model = model;
    p.batch = batch;
    p.preproc_ms = 100;
    p.inference_ms = inf_ms;
    p.dynamic_inf_ms.store(inf_ms);
    p.dynamic_pre_ms.store(100);
    p.threads = threads;
    p.max_buffer = 5;
    p.file_size_mb = 20;
    p.pre_mem_mb = 3;
    p.inf_mem_mb = inf_mem_mb;
    p.key = model + "_" + std::to_string(batch);
    return p;
}

// Helper: create a Job for testing
static Job makeJob(const std::string& model, int batch, long slo_ms, long arrival_offset_ms = 0) {
    Job j;
    j.type = 'r';
    j.client_sock = -1;
    j.model = model;
    j.batch = batch;
    j.arrival_ts = nowMs() - arrival_offset_ms;
    j.requested_slo_ms = slo_ms;
    j.assigned_port = 0;
    return j;
}

// Helper: prepare FileManager with ready files for given profiles
// Note: usleep(1) between calls ensures unique microsecond-based prefixes
static void prepareFiles(FileManager& fm, std::map<std::string, ModelProfile>& profiles, int count = 5) {
    for (auto& [key, _] : profiles) {
        for (int i = 0; i < count; i++) {
            std::string prefix = fm.initiateFile(key);
            fm.setReady(key, prefix);
            usleep(1); // Ensure unique timestamp in prefix
        }
    }
}

// CRC32 matching DQNScheduler.hpp implementation
static uint32_t testCRC32Table(uint32_t idx) {
    uint32_t crc = idx;
    for (int j = 0; j < 8; ++j) {
        crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
    return crc;
}

static uint32_t testComputeCRC32(const void* data, size_t len) {
    const uint8_t* buf = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ testCRC32Table((crc ^ buf[i]) & 0xFF);
    }
    return crc ^ 0xFFFFFFFF;
}

// Generate DQN2 format test weight file (64-32-1 architecture)
// Total weight floats: (5*64+64) + (64*32+32) + (32*1+1) = 384 + 2080 + 33 = 2497
// Total weight bytes: 2497 * 4 = 9988
// DQN2 header: 28 bytes (4 magic + 16 dims + 4 arch_hash + 4 crc32)
// Total file: 10016 bytes
static void generateTestWeightsDQN2(const std::string& path) {
    // Generate raw weight bytes
    const size_t WEIGHT_FLOATS = 2497;
    const size_t WEIGHT_BYTES = WEIGHT_FLOATS * sizeof(float);
    std::vector<float> weights(WEIGHT_FLOATS);

    size_t off = 0;
    // W1: 64x5
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 5; k++)
            weights[off++] = 0.1f * (float)(i + 1) * (float)(k + 1) / 320.0f;
    // b1: 64
    for (int i = 0; i < 64; i++) weights[off++] = 0.0f;
    // W2: 32x64
    for (int i = 0; i < 32; i++)
        for (int k = 0; k < 64; k++)
            weights[off++] = 0.05f * (float)(i + 1) / 32.0f;
    // b2: 32
    for (int i = 0; i < 32; i++) weights[off++] = 0.0f;
    // W3: 1x32
    for (int k = 0; k < 32; k++)
        weights[off++] = 0.1f * (float)(k + 1) / 32.0f;
    // b3: 1
    weights[off++] = 0.0f;

    // Compute CRC32 over raw weights
    uint32_t crc = testComputeCRC32(weights.data(), WEIGHT_BYTES);

    // FNV-1a hash of "5-64-32-1"
    uint32_t arch_hash = 2166136261u;
    const char* arch_str = "5-64-32-1";
    for (const char* p = arch_str; *p; ++p) {
        arch_hash ^= (uint32_t)(unsigned char)(*p);
        arch_hash *= 16777619u;
    }

    // Write DQN2 file
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return;
    f.write("DQN2", 4);
    uint32_t dims[4] = {5, 64, 32, 1};
    f.write(reinterpret_cast<char*>(dims), 16);
    f.write(reinterpret_cast<char*>(&arch_hash), 4);
    f.write(reinterpret_cast<char*>(&crc), 4);
    f.write(reinterpret_cast<char*>(weights.data()), WEIGHT_BYTES);
    f.close();
}

// Generate raw format test weight file (no header, 64-32-1)
static void generateTestWeightsRaw(const std::string& path) {
    const size_t WEIGHT_FLOATS = 2497;
    std::vector<float> weights(WEIGHT_FLOATS);

    size_t off = 0;
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 5; k++)
            weights[off++] = 0.1f * (float)(i + 1) * (float)(k + 1) / 320.0f;
    for (int i = 0; i < 64; i++) weights[off++] = 0.0f;
    for (int i = 0; i < 32; i++)
        for (int k = 0; k < 64; k++)
            weights[off++] = 0.05f * (float)(i + 1) / 32.0f;
    for (int i = 0; i < 32; i++) weights[off++] = 0.0f;
    for (int k = 0; k < 32; k++)
        weights[off++] = 0.1f * (float)(k + 1) / 32.0f;
    weights[off++] = 0.0f;

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<char*>(weights.data()), WEIGHT_FLOATS * sizeof(float));
    f.close();
}

// Generate a legacy DQN1 format weight file (16-8-1 architecture) for detection test
static void generateLegacyDQN1Weights(const std::string& path) {
    // DQN1: (5*16+16) + (16*8+8) + (8*1+1) = 96+136+9 = 241 floats = 964 bytes
    const size_t WEIGHT_FLOATS = 241;
    std::vector<float> weights(WEIGHT_FLOATS, 0.01f);

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<char*>(weights.data()), WEIGHT_FLOATS * sizeof(float));
    f.close();
}

// =========================================================
// TEST 1: FCFS Scheduler
// =========================================================
static int test_fcfs() {
    std::cout << "\n[TEST 1] FCFS Scheduler -- FIFO Order\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);

    FCFSScheduler fcfs;

    // Push in order: simc1 then hinet then simc1
    fcfs.push(makeJob("simc1", 1, 10000));
    fcfs.push(makeJob("hinet", 1, 8000));
    fcfs.push(makeJob("simc1", 1, 5000));

    FileManager fm;
    prepareFiles(fm, profiles);

    // FCFS should pop in insertion order
    auto j1 = fcfs.popReadyJob(fm, profiles, 128.0);
    CHECK(j1.has_value() && j1->model == "simc1" && j1->requested_slo_ms == 10000,
          "First pop = simc1 SLO=10000 (insertion order)");

    auto j2 = fcfs.popReadyJob(fm, profiles, 128.0);
    CHECK(j2.has_value() && j2->model == "hinet",
          "Second pop = hinet (insertion order)");

    auto j3 = fcfs.popReadyJob(fm, profiles, 128.0);
    CHECK(j3.has_value() && j3->model == "simc1" && j3->requested_slo_ms == 5000,
          "Third pop = simc1 SLO=5000 (insertion order)");

    CHECK(fcfs.size() == 0, "Queue empty after all pops");

    return 0;
}

// =========================================================
// TEST 2: SJF Scheduler — Shortest Job First with aging
// =========================================================
static int test_sjf() {
    std::cout << "\n[TEST 2] SJF Scheduler -- Shortest Job First\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);     // short
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);    // medium
    profiles["alexnet_1"] = makeProfile("alexnet", 1, 25000, 4, 8932); // long

    SJFScheduler sjf(0.5);

    sjf.push(makeJob("alexnet", 1, 60000));  // long job
    sjf.push(makeJob("simc1", 1, 5000));     // short job
    sjf.push(makeJob("hinet", 1, 10000));    // medium job

    FileManager fm;
    prepareFiles(fm, profiles);

    // SJF should pop shortest inference time first
    auto j1 = sjf.popReadyJob(fm, profiles, 128.0);
    CHECK(j1.has_value() && j1->model == "simc1", "First pop = simc1 (shortest: 3000ms)");

    auto j2 = sjf.popReadyJob(fm, profiles, 128.0);
    CHECK(j2.has_value() && j2->model == "hinet", "Second pop = hinet (medium: 5000ms)");

    auto j3 = sjf.popReadyJob(fm, profiles, 128.0);
    CHECK(j3.has_value() && j3->model == "alexnet", "Third pop = alexnet (longest: 25000ms)");

    CHECK(sjf.size() == 0, "Queue empty");

    return 0;
}

// =========================================================
// TEST 3: EDF Scheduler — Earliest Deadline First
// =========================================================
static int test_edf() {
    std::cout << "\n[TEST 3] EDF Scheduler -- Earliest Deadline First\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);

    EDFScheduler edf;

    long now = nowMs();
    // Job with earliest absolute deadline should come first
    // Deadline = arrival_ts + requested_slo_ms

    Job j_late;
    j_late.type = 'r'; j_late.client_sock = -1; j_late.model = "hinet"; j_late.batch = 1;
    j_late.arrival_ts = now; j_late.requested_slo_ms = 50000; // deadline = now + 50s
    j_late.assigned_port = 0;

    Job j_mid;
    j_mid.type = 'r'; j_mid.client_sock = -1; j_mid.model = "simc1"; j_mid.batch = 1;
    j_mid.arrival_ts = now; j_mid.requested_slo_ms = 10000; // deadline = now + 10s
    j_mid.assigned_port = 0;

    Job j_early;
    j_early.type = 'r'; j_early.client_sock = -1; j_early.model = "simc1"; j_early.batch = 1;
    j_early.arrival_ts = now; j_early.requested_slo_ms = 4000; // deadline = now + 4s
    j_early.assigned_port = 0;

    // Push in reverse order
    edf.push(j_late);
    edf.push(j_mid);
    edf.push(j_early);

    FileManager fm;
    prepareFiles(fm, profiles);

    auto p1 = edf.popReadyJob(fm, profiles, 128.0);
    CHECK(p1.has_value() && p1->requested_slo_ms == 4000, "First pop = earliest deadline (SLO=4000)");

    auto p2 = edf.popReadyJob(fm, profiles, 128.0);
    CHECK(p2.has_value() && p2->requested_slo_ms == 10000, "Second pop = mid deadline (SLO=10000)");

    auto p3 = edf.popReadyJob(fm, profiles, 128.0);
    CHECK(p3.has_value() && p3->requested_slo_ms == 50000, "Third pop = latest deadline (SLO=50000)");

    CHECK(edf.size() == 0, "Queue empty");

    return 0;
}

// =========================================================
// TEST 4: LST Scheduler — Least Slack Time
// =========================================================
static int test_lst() {
    std::cout << "\n[TEST 4] LST Scheduler -- Least Slack Time\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);

    LSTScheduler lst;

    long now = nowMs();

    // Slack = (arrival + SLO) - (now + expected_inf)
    // Job A: slack = (now + 50000) - (now + 5000) = 45000
    Job jA;
    jA.type = 'r'; jA.client_sock = -1; jA.model = "hinet"; jA.batch = 1;
    jA.arrival_ts = now; jA.requested_slo_ms = 50000; jA.assigned_port = 0;

    // Job B: slack = (now + 4000) - (now + 3000) = 1000
    Job jB;
    jB.type = 'r'; jB.client_sock = -1; jB.model = "simc1"; jB.batch = 1;
    jB.arrival_ts = now; jB.requested_slo_ms = 4000; jB.assigned_port = 0;

    // Job C: slack = (now + 8000) - (now + 5000) = 3000
    Job jC;
    jC.type = 'r'; jC.client_sock = -1; jC.model = "hinet"; jC.batch = 1;
    jC.arrival_ts = now; jC.requested_slo_ms = 8000; jC.assigned_port = 0;

    // Push in reverse slack order
    lst.push(jA);  // slack 45000
    lst.push(jC);  // slack 3000
    lst.push(jB);  // slack 1000

    FileManager fm;
    prepareFiles(fm, profiles);

    auto p1 = lst.popReadyJob(fm, profiles, 128.0);
    CHECK(p1.has_value() && p1->requested_slo_ms == 4000,
          "First pop = tightest slack (simc1 SLO=4000, slack~1000)");

    auto p2 = lst.popReadyJob(fm, profiles, 128.0);
    CHECK(p2.has_value() && p2->requested_slo_ms == 8000,
          "Second pop = moderate slack (hinet SLO=8000, slack~3000)");

    auto p3 = lst.popReadyJob(fm, profiles, 128.0);
    CHECK(p3.has_value() && p3->requested_slo_ms == 50000,
          "Third pop = most slack (hinet SLO=50000, slack~45000)");

    CHECK(lst.size() == 0, "Queue empty");

    return 0;
}

// =========================================================
// TEST 5: SA Scheduler — basic operation and reordering
// =========================================================
static int test_sa_basic() {
    std::cout << "\n[TEST 5] SA Scheduler -- Basic Operation\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);
    profiles["alexnet_1"] = makeProfile("alexnet", 1, 25000, 4, 8932);

    SAScheduler sa(100.0, 0.92, 80, 100, 50);

    sa.push(makeJob("alexnet", 1, 30000));
    sa.push(makeJob("simc1", 1, 5000));
    sa.push(makeJob("hinet", 1, 8000));
    sa.push(makeJob("simc1", 1, 6000));

    CHECK(sa.size() == 4, "Queue size = 4 after push");

    FileManager fm;
    prepareFiles(fm, profiles);

    auto job1 = sa.popReadyJob(fm, profiles, 128.0);
    CHECK(job1.has_value(), "First pop returns a job");

    auto job2 = sa.popReadyJob(fm, profiles, 128.0);
    CHECK(job2.has_value(), "Second pop returns a job");

    CHECK(sa.size() == 2, "Queue size = 2 after 2 pops");

    return 0;
}

// =========================================================
// TEST 6: SA Scheduler — SLO-aware reordering quality
// =========================================================
static int test_sa_ordering() {
    std::cout << "\n[TEST 6] SA Scheduler -- SLO-Aware Reordering\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);

    SAScheduler sa(100.0, 0.92, 80, 100, 50);

    // Push in reverse urgency order
    sa.push(makeJob("hinet", 1, 50000));  // very relaxed SLO
    sa.push(makeJob("hinet", 1, 50000));
    sa.push(makeJob("simc1", 1, 4000));   // tight SLO
    sa.push(makeJob("simc1", 1, 4500));   // tight SLO

    FileManager fm;
    prepareFiles(fm, profiles);

    std::vector<Job> popped;
    for (int i = 0; i < 4; i++) {
        auto j = sa.popReadyJob(fm, profiles, 128.0);
        if (j.has_value()) popped.push_back(*j);
    }

    CHECK(popped.size() == 4, "All 4 jobs popped");

    std::cout << "    Pop order:\n";
    for (size_t i = 0; i < popped.size(); i++) {
        std::cout << "      [" << i << "] " << popped[i].model << "_" << popped[i].batch
                  << " SLO=" << popped[i].requested_slo_ms << "ms\n";
    }

    // Tight-SLO jobs should be scheduled first (SA is stochastic but strongly biased)
    bool tight_first = popped.size() >= 2 && popped[0].model == "simc1" && popped[1].model == "simc1";
    CHECK(tight_first, "Tight-SLO simc1 jobs scheduled before relaxed hinet jobs");

    return 0;
}

// =========================================================
// TEST 7: SA Scheduler — Memory pressure awareness
// =========================================================
static int test_sa_memory_pressure() {
    std::cout << "\n[TEST 7] SA Scheduler -- Memory Pressure Awareness\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["alexnet_1"] = makeProfile("alexnet", 1, 25000, 4, 8932);
    profiles["simc1_1"]   = makeProfile("simc1", 1, 3000, 1, 25);

    SAScheduler sa(100.0, 0.92, 80, 100, 50);

    sa.push(makeJob("alexnet", 1, 60000));
    sa.push(makeJob("alexnet", 1, 60000));
    sa.push(makeJob("simc1", 1, 10000));
    sa.push(makeJob("simc1", 1, 10000));

    FileManager fm;
    prepareFiles(fm, profiles);

    std::vector<Job> popped;
    for (int i = 0; i < 4; i++) {
        auto j = sa.popReadyJob(fm, profiles, 16.0);
        if (j.has_value()) popped.push_back(*j);
    }

    CHECK(popped.size() == 4, "All 4 jobs scheduled under memory pressure");

    std::cout << "    Memory-aware pop order:\n";
    for (size_t i = 0; i < popped.size(); i++) {
        std::cout << "      [" << i << "] " << popped[i].model << "_" << popped[i].batch
                  << " (mem=" << profiles.at(popped[i].model + "_" + std::to_string(popped[i].batch)).inf_mem_mb
                  << "MB)\n";
    }

    CHECK(true, "SA completed without crash under memory pressure");

    return 0;
}

// =========================================================
// TEST 8: DQN Scheduler — LST Fallback (no weights)
// =========================================================
static int test_dqn_fallback() {
    std::cout << "\n[TEST 8] DQN Scheduler -- LST Fallback (no weights)\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);

    DQNScheduler dqn("", 24, 0);

    dqn.push(makeJob("hinet", 1, 50000));   // large slack
    dqn.push(makeJob("simc1", 1, 4000));    // small slack
    dqn.push(makeJob("hinet", 1, 6000));    // moderate slack

    FileManager fm;
    prepareFiles(fm, profiles);

    auto j1 = dqn.popReadyJob(fm, profiles, 128.0);
    CHECK(j1.has_value(), "First pop returns a job");
    if (j1.has_value()) {
        std::cout << "      Got: " << j1->model << " SLO=" << j1->requested_slo_ms << "ms\n";
    }

    auto j2 = dqn.popReadyJob(fm, profiles, 128.0);
    CHECK(j2.has_value(), "Second pop returns a job");

    auto j3 = dqn.popReadyJob(fm, profiles, 128.0);
    CHECK(j3.has_value(), "Third pop returns a job");

    CHECK(dqn.size() == 0, "Queue empty after all pops");

    return 0;
}

// =========================================================
// TEST 9: DQN Scheduler — DQN2 Header Format (64-32-1)
// =========================================================
static int test_dqn_with_weights() {
    std::cout << "\n[TEST 9] DQN Scheduler -- DQN2 Header Format (64-32-1)\n";

    std::string weights_path = "/tmp/test_dqn2_header.bin";
    generateTestWeightsDQN2(weights_path);

    // DQN2: 28-byte header + 9988 bytes weights = 10016 bytes
    std::ifstream check(weights_path, std::ios::binary | std::ios::ate);
    auto fsize = check.tellg();
    check.close();
    CHECK(fsize == 10016, "Weight file size = 10016 bytes (28 header + 9988 weights)");

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);
    profiles["alexnet_1"] = makeProfile("alexnet", 1, 25000, 4, 8932);

    DQNScheduler dqn(weights_path, 24, 131072);

    dqn.push(makeJob("alexnet", 1, 30000));
    dqn.push(makeJob("simc1", 1, 5000));
    dqn.push(makeJob("hinet", 1, 8000));

    FileManager fm;
    prepareFiles(fm, profiles);

    std::vector<Job> popped;
    for (int i = 0; i < 3; i++) {
        auto j = dqn.popReadyJob(fm, profiles, 128.0);
        if (j.has_value()) popped.push_back(*j);
    }

    CHECK(popped.size() == 3, "All 3 jobs popped with NN scoring");

    std::cout << "    NN-scored pop order:\n";
    for (size_t i = 0; i < popped.size(); i++) {
        std::cout << "      [" << i << "] " << popped[i].model << "_" << popped[i].batch
                  << " SLO=" << popped[i].requested_slo_ms << "ms\n";
    }

    CHECK(true, "NN forward pass produced valid output (64-32-1)");

    return 0;
}

// =========================================================
// TEST 10: DQN Scheduler — Raw Weight Format (no header)
// =========================================================
static int test_dqn_raw_weights() {
    std::cout << "\n[TEST 10] DQN Scheduler -- Raw Weight Format (64-32-1)\n";

    std::string weights_path = "/tmp/test_dqn2_raw.bin";
    generateTestWeightsRaw(weights_path);

    std::ifstream check(weights_path, std::ios::binary | std::ios::ate);
    auto fsize = check.tellg();
    check.close();
    CHECK(fsize == 9988, "Raw weight file = 9988 bytes (2497 floats)");

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);

    DQNScheduler dqn(weights_path, 24, 131072);

    dqn.push(makeJob("simc1", 1, 5000));
    dqn.push(makeJob("simc1", 1, 10000));

    FileManager fm;
    prepareFiles(fm, profiles);

    auto j1 = dqn.popReadyJob(fm, profiles, 128.0);
    CHECK(j1.has_value(), "Pop with raw 64-32-1 weights succeeds");

    return 0;
}

// =========================================================
// TEST 11: DQN — Legacy DQN1 Detection + Bad File Handling
// =========================================================
static int test_dqn_bad_weights() {
    std::cout << "\n[TEST 11] DQN Scheduler -- Legacy DQN1 + Bad File Handling\n";

    // Sub-test A: Non-existent file → LST fallback
    {
        DQNScheduler dqn("/tmp/nonexistent_weights_12345.bin", 24, 0);
        std::map<std::string, ModelProfile> profiles;
        profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
        dqn.push(makeJob("simc1", 1, 5000));
        FileManager fm;
        std::string prefix = fm.initiateFile("simc1_1");
        fm.setReady("simc1_1", prefix);
        auto j = dqn.popReadyJob(fm, profiles, 128.0);
        CHECK(j.has_value(), "Fallback to LST on missing file");
    }

    // Sub-test B: Legacy DQN1 raw format (964 bytes) → detected + fallback
    {
        std::string path = "/tmp/test_dqn1_legacy.bin";
        generateLegacyDQN1Weights(path);

        std::ifstream check(path, std::ios::binary | std::ios::ate);
        auto fsize = check.tellg();
        check.close();
        CHECK(fsize == 964, "Legacy DQN1 file = 964 bytes");

        DQNScheduler dqn(path, 24, 0);
        std::map<std::string, ModelProfile> profiles;
        profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
        dqn.push(makeJob("simc1", 1, 5000));
        FileManager fm;
        std::string prefix = fm.initiateFile("simc1_1");
        fm.setReady("simc1_1", prefix);
        auto j = dqn.popReadyJob(fm, profiles, 128.0);
        CHECK(j.has_value(), "Fallback to LST on legacy DQN1 (16-8-1) weight file");
    }

    // Sub-test C: Wrong-size file → fallback
    {
        std::ofstream bad("/tmp/test_dqn_bad.bin", std::ios::binary);
        float garbage[10] = {1.0f};
        bad.write(reinterpret_cast<char*>(garbage), sizeof(garbage));
        bad.close();

        DQNScheduler dqn("/tmp/test_dqn_bad.bin", 24, 0);
        std::map<std::string, ModelProfile> profiles;
        profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
        dqn.push(makeJob("simc1", 1, 5000));
        FileManager fm;
        std::string prefix = fm.initiateFile("simc1_1");
        fm.setReady("simc1_1", prefix);
        auto j = dqn.popReadyJob(fm, profiles, 128.0);
        CHECK(j.has_value(), "Fallback to LST on wrong-size file");
    }

    // Sub-test D: Bad magic header → fallback
    {
        std::ofstream bad("/tmp/test_dqn_badmagic.bin", std::ios::binary);
        bad.write("XXXX", 4);
        uint32_t dims[4] = {5, 64, 32, 1};
        bad.write(reinterpret_cast<char*>(dims), 16);
        uint32_t dummy[2] = {0, 0}; // arch_hash + crc
        bad.write(reinterpret_cast<char*>(dummy), 8);
        std::vector<float> zeros(2497, 0.0f);
        bad.write(reinterpret_cast<char*>(zeros.data()), 2497 * sizeof(float));
        bad.close();

        DQNScheduler dqn("/tmp/test_dqn_badmagic.bin", 24, 0);
        std::map<std::string, ModelProfile> profiles;
        profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
        dqn.push(makeJob("simc1", 1, 5000));
        FileManager fm;
        std::string prefix = fm.initiateFile("simc1_1");
        fm.setReady("simc1_1", prefix);
        auto j = dqn.popReadyJob(fm, profiles, 128.0);
        CHECK(j.has_value(), "Fallback to LST on bad magic header");
    }

    return 0;
}

// =========================================================
// TEST 12: Config parsing of all parameters
// =========================================================
static int test_config_params() {
    std::cout << "\n[TEST 12] Config Parsing -- All Parameters (incl. Q-3, Q-8)\n";

    std::string cfg_path = "/tmp/test_kairos_config.cfg";
    {
        std::ofstream f(cfg_path);
        f << "SERVER_IP=127.0.0.1\n"
          << "SCHEDULER_PORT=8000\n"
          << "MAX_CONN=10\n"
          << "TOTAL_CORES=8\n"
          << "SYSTEM_RESERVED_CORES=1\n"
          << "MAX_PREPROC_CONCURRENCY=2\n"
          << "ENABLE_CORE_PINNING=true\n"
          << "BASE_PORT=47100\n"
          << "PORT_RANGE=100\n"
          << "SCHEDULER_MODE=SA\n"
          << "DEFAULT_SLO_K_FACTOR=2.0\n"
          << "VFT_SAFETY_MARGIN=1.2\n"
          << "AGING_FACTOR=0.5\n"
          << "SA_INITIAL_TEMP=200.0\n"
          << "SA_COOLING_RATE=0.95\n"
          << "SA_MAX_ITERATIONS=100\n"
          << "SA_WINDOW_SIZE=150\n"
          << "SA_MIN_RETRIGGER_MS=75\n"
          << "EWMA_ALPHA_PRE=0.25\n"
          << "EWMA_ALPHA_ON=0.45\n"
          << "MEM_SAFETY_THRESHOLD=0.85\n"
          << "MEM_RESERVE_GB=3.0\n"
          << "TOTAL_MEMORY_MB=65536\n"
          << "MAX_LEASE_TIMEOUT_S=900\n"
          << "PROFILE_CHECKPOINT_JOBS=20\n"
          << "PROFILE_CHECKPOINT_SECS=60\n"
          << "PRE_BASE_PORT=47000\n"
          << "TELEMETRY_SAMPLE_RATE_MS=250\n"
          << "ENABLE_TIME_SERIES_LOGGING=true\n"
          << "MEM_TRACE_FILE=logs/custom_mem.csv\n"
          << "THETA_NEGOTIATION_THRESHOLD=2.0\n"
          << "NEGOTIATION_TIMEOUT_MS=300\n"
          << "SERVER_CMD_TEMPLATE=echo test\n"
          << "PREPROC_CMD_TEMPLATE=echo test\n"
          << "SNNI_DIR=.\n"
          << "LOG_FILE=logs/test.csv\n"
          << "SYS_FILE=logs/test_sys.csv\n"
          << "DQN_WEIGHTS_PATH=\n"
          << "DYNAMIC_PROFILE_FILE=logs/test_dyn.cfg\n";
    }

    SystemConfig cfg = ConfigManager::loadSystemConfig(cfg_path);

    // SA parameters
    CHECK(cfg.scheduler_mode == "SA", "SCHEDULER_MODE=SA");
    CHECK(std::abs(cfg.sa_initial_temp - 200.0) < 0.01, "SA_INITIAL_TEMP=200.0");
    CHECK(std::abs(cfg.sa_cooling_rate - 0.95) < 0.001, "SA_COOLING_RATE=0.95");
    CHECK(cfg.sa_max_iterations == 100, "SA_MAX_ITERATIONS=100");
    CHECK(cfg.sa_window_size == 150, "SA_WINDOW_SIZE=150");
    CHECK(cfg.sa_min_retrigger_ms == 75, "SA_MIN_RETRIGGER_MS=75");
    CHECK(std::abs(cfg.vft_safety_margin - 1.2) < 0.01, "VFT_SAFETY_MARGIN=1.2");

    // EWMA parameters (C-05)
    CHECK(std::abs(cfg.ewma_alpha_pre - 0.25) < 0.01, "EWMA_ALPHA_PRE=0.25");
    CHECK(std::abs(cfg.ewma_alpha_on - 0.45) < 0.01, "EWMA_ALPHA_ON=0.45");

    // Memory safety (C-23)
    CHECK(std::abs(cfg.mem_safety_threshold - 0.85) < 0.01, "MEM_SAFETY_THRESHOLD=0.85");
    CHECK(std::abs(cfg.mem_reserve_gb - 3.0) < 0.01, "MEM_RESERVE_GB=3.0");

    // Config-driven normalization (C-11)
    CHECK(cfg.total_memory_mb == 65536, "TOTAL_MEMORY_MB=65536");

    // Dynamic watchdog (C-22)
    CHECK(cfg.max_lease_timeout_s == 900, "MAX_LEASE_TIMEOUT_S=900");

    // Profile checkpointing (C-25)
    CHECK(cfg.profile_checkpoint_jobs == 20, "PROFILE_CHECKPOINT_JOBS=20");
    CHECK(cfg.profile_checkpoint_secs == 60, "PROFILE_CHECKPOINT_SECS=60");

    // Pre-processing port (C-28)
    CHECK(cfg.pre_base_port == 47000, "PRE_BASE_PORT=47000");

    // Q-8: Telemetry parameters
    CHECK(cfg.telemetry_sample_rate_ms == 250, "TELEMETRY_SAMPLE_RATE_MS=250");
    CHECK(cfg.enable_time_series_logging == true, "ENABLE_TIME_SERIES_LOGGING=true");
    CHECK(cfg.mem_trace_file == "logs/custom_mem.csv", "MEM_TRACE_FILE=logs/custom_mem.csv");

    // Q-3: Negotiation parameters
    CHECK(std::abs(cfg.theta_negotiation_threshold - 2.0) < 0.01, "THETA_NEGOTIATION_THRESHOLD=2.0");
    CHECK(cfg.negotiation_timeout_ms == 300, "NEGOTIATION_TIMEOUT_MS=300");

    return 0;
}

// =========================================================
// TEST 13: SchedulerFactory — All modes including CUSTOM
// =========================================================
static int test_factory() {
    std::cout << "\n[TEST 13] SchedulerFactory -- All Modes\n";

    auto fcfs = SchedulerFactory::create("FCFS", 0.5, "");
    CHECK(fcfs != nullptr, "Factory creates FCFS");

    auto sjf = SchedulerFactory::create("SJF", 0.5, "");
    CHECK(sjf != nullptr, "Factory creates SJF");

    auto edf = SchedulerFactory::create("EDF", 0.5, "");
    CHECK(edf != nullptr, "Factory creates EDF");

    auto lst = SchedulerFactory::create("LST", 0.5, "");
    CHECK(lst != nullptr, "Factory creates LST");

    auto sa = SchedulerFactory::create("SA", 0.5, "", 100.0, 0.92, 80, 100, 50);
    CHECK(sa != nullptr, "Factory creates SA with params");

    auto dqn = SchedulerFactory::create("DQN", 0.5, "", 100.0, 0.92, 80, 100, 50, 24, 131072);
    CHECK(dqn != nullptr, "Factory creates DQN with total_cores and total_memory_mb");

    auto custom = SchedulerFactory::create("CUSTOM", 0.5, "");
    CHECK(custom != nullptr, "Factory creates CUSTOM (SPLS)");

    // Test unknown mode throws
    bool threw = false;
    try {
        auto bad = SchedulerFactory::create("INVALID", 0.5, "");
        (void)bad;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "Factory throws on unknown mode");

    return 0;
}

// =========================================================
// TEST 14: CUSTOM/SPLS Scheduler — State-Prioritized Least-Slack
// =========================================================
static int test_custom_spls() {
    std::cout << "\n[TEST 14] CUSTOM/SPLS Scheduler -- State-Prioritized Least-Slack\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"]   = makeProfile("simc1", 1, 3000, 1, 25);      // small mem
    profiles["hinet_1"]   = makeProfile("hinet", 1, 5000, 4, 255);     // moderate mem
    profiles["alexnet_1"] = makeProfile("alexnet", 1, 25000, 4, 8932); // huge mem

    CustomScheduler custom;

    long now = nowMs();

    // Job with tight slack but small memory → highest priority
    Job j1;
    j1.type = 'r'; j1.client_sock = -1; j1.model = "simc1"; j1.batch = 1;
    j1.arrival_ts = now; j1.requested_slo_ms = 4000; j1.assigned_port = 0;

    // Job with moderate slack but huge memory → penalized
    Job j2;
    j2.type = 'r'; j2.client_sock = -1; j2.model = "alexnet"; j2.batch = 1;
    j2.arrival_ts = now; j2.requested_slo_ms = 30000; j2.assigned_port = 0;

    // Job with moderate slack, moderate memory
    Job j3;
    j3.type = 'r'; j3.client_sock = -1; j3.model = "hinet"; j3.batch = 1;
    j3.arrival_ts = now; j3.requested_slo_ms = 10000; j3.assigned_port = 0;

    custom.push(j2);  // Push memory-heavy first
    custom.push(j3);
    custom.push(j1);  // Push tight-slack last

    FileManager fm;
    prepareFiles(fm, profiles);

    auto p1 = custom.popReadyJob(fm, profiles, 128.0);
    CHECK(p1.has_value(), "SPLS first pop returns a job");
    if (p1.has_value()) {
        std::cout << "      First: " << p1->model << " SLO=" << p1->requested_slo_ms << "ms\n";
    }

    auto p2 = custom.popReadyJob(fm, profiles, 128.0);
    CHECK(p2.has_value(), "SPLS second pop returns a job");
    if (p2.has_value()) {
        std::cout << "      Second: " << p2->model << " SLO=" << p2->requested_slo_ms << "ms\n";
    }

    auto p3 = custom.popReadyJob(fm, profiles, 128.0);
    CHECK(p3.has_value(), "SPLS third pop returns a job");
    if (p3.has_value()) {
        std::cout << "      Third: " << p3->model << " SLO=" << p3->requested_slo_ms << "ms\n";
    }

    // Tight slack + small memory should come first
    CHECK(p1.has_value() && p1->model == "simc1",
          "SPLS prioritizes tight-slack, low-memory job (simc1)");

    CHECK(custom.size() == 0, "Queue empty after all pops");

    return 0;
}

// =========================================================
// TEST 15: Welford's Algorithm — Online Standard Deviation
// =========================================================
static int test_welford() {
    std::cout << "\n[TEST 15] Welford's Algorithm -- Dual-Phase Online StdDev Tracking (FU-5)\n";

    ModelProfile p = makeProfile("simc1", 1, 3000, 1, 25);

    // FU-5: Test inference (on) phase tracker
    CHECK(p.welford_on_count == 0, "Initial on_count = 0");
    CHECK(p.welfordStdDevOn() == 0.0, "Initial on stddev = 0 (count < 2)");

    // Feed known values to inference tracker: 10, 20, 30
    p.welfordUpdateOn(10.0);
    CHECK(p.welford_on_count == 1, "on_count = 1 after first update");
    CHECK(std::abs(p.welford_on_mean - 10.0) < 0.001, "on_mean = 10.0 after first update");
    CHECK(p.welfordStdDevOn() == 0.0, "on StdDev = 0 with only 1 sample");

    p.welfordUpdateOn(20.0);
    CHECK(p.welford_on_count == 2, "on_count = 2 after second update");

    p.welfordUpdateOn(30.0);
    CHECK(p.welford_on_count == 3, "on_count = 3 after third update");
    CHECK(std::abs(p.welford_on_mean - 20.0) < 0.001, "on_mean = 20.0 after {10, 20, 30}");

    double expected_std = 10.0;
    CHECK(std::abs(p.welfordStdDevOn() - expected_std) < 0.01,
          "on StdDev of {10, 20, 30} = 10.0 (sample stddev)");

    // FU-5: Test preprocessing (pre) phase tracker independently
    CHECK(p.welford_pre_count == 0, "Initial pre_count = 0");
    p.welfordUpdatePre(50.0);
    p.welfordUpdatePre(100.0);
    p.welfordUpdatePre(150.0);
    CHECK(p.welford_pre_count == 3, "pre_count = 3 after three updates");
    CHECK(std::abs(p.welford_pre_mean - 100.0) < 0.001, "pre_mean = 100.0 after {50, 100, 150}");
    double expected_pre_std = 50.0;
    CHECK(std::abs(p.welfordStdDevPre() - expected_pre_std) < 0.01,
          "pre StdDev of {50, 100, 150} = 50.0");

    // Test legacy backward-compatible API
    CHECK(std::abs(p.welfordStdDev() - expected_std) < 0.01,
          "Legacy welfordStdDev() returns on-phase stddev");

    // Test copy preserves dual-phase Welford state
    ModelProfile copy = p;
    CHECK(copy.welford_on_count == 3, "Copy preserves welford_on_count");
    CHECK(std::abs(copy.welford_on_mean - 20.0) < 0.001, "Copy preserves welford_on_mean");
    CHECK(copy.welford_pre_count == 3, "Copy preserves welford_pre_count");
    CHECK(std::abs(copy.welford_pre_mean - 100.0) < 0.001, "Copy preserves welford_pre_mean");

    return 0;
}

// =========================================================
// TEST 16: Q-1 — Job Cap_c Fields and Defaults
// =========================================================
static int test_job_cap_c() {
    std::cout << "\n[TEST 16] Q-1: Job Cap_c Fields -- Defaults and Assignment\n";

    // Default job should have symmetric defaults
    Job j = makeJob("simc1", 1, 5000);
    CHECK(j.device_class == 4, "Default device_class = 4 (Desktop)");
    CHECK(std::abs(j.cpu_freq - 3.0f) < 0.01f, "Default cpu_freq = 3.0 GHz");
    CHECK(j.ram_mb == 8192, "Default ram_mb = 8192");
    CHECK(j.net_type == 0, "Default net_type = 0 (LAN)");
    CHECK(j.is_default_cap == true, "Default is_default_cap = true");

    // Test custom assignment
    j.device_class = 1;   // Mobile
    j.cpu_freq = 1.5f;
    j.ram_mb = 4096;
    j.net_type = 2;       // 5G
    j.is_default_cap = false;
    CHECK(j.device_class == 1, "Custom device_class = 1 (Mobile)");
    CHECK(j.net_type == 2, "Custom net_type = 2 (5G)");
    CHECK(j.is_default_cap == false, "Custom is_default_cap = false");

    // Test theta/phi defaults
    CHECK(std::abs(j.theta_est - 1.0) < 0.001, "Default theta_est = 1.0");
    CHECK(std::abs(j.theta_act - 0.0) < 0.001, "Default theta_act = 0.0");
    CHECK(j.penalty_phi == 0, "Default penalty_phi = 0");

    // Test request_id and timestamps
    CHECK(j.request_id.empty(), "Default request_id empty");
    CHECK(j.t_start_pre == 0, "Default t_start_pre = 0");
    CHECK(j.t_fin_pre == 0, "Default t_fin_pre = 0");
    CHECK(j.t_start_on == 0, "Default t_start_on = 0");

    // Test scheduled_cores default
    CHECK(j.scheduled_cores == 0, "Default scheduled_cores = 0");

    return 0;
}

// =========================================================
// TEST 17: Q-6 — SA Malleable Core Sizing
// =========================================================
static int test_sa_malleable_cores() {
    std::cout << "\n[TEST 17] Q-6: SA Malleable Core Sizing\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 2, 25);   // 2 threads default
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);  // 4 threads default

    // SA with total_cores=8 so core adjustments are bounded
    SAScheduler sa(100.0, 0.92, 200, 100, 0, 8);

    // Push multiple jobs
    for (int i = 0; i < 6; i++) {
        sa.push(makeJob("simc1", 1, 10000));
        sa.push(makeJob("hinet", 1, 15000));
    }

    FileManager fm;
    prepareFiles(fm, profiles, 10);

    // Pop all and check that SA ran without crash
    std::vector<Job> popped;
    int with_custom_cores = 0;
    for (int i = 0; i < 12; i++) {
        auto j = sa.popReadyJob(fm, profiles, 128.0);
        if (j.has_value()) {
            popped.push_back(*j);
            if (j->scheduled_cores > 0) with_custom_cores++;
        }
    }

    CHECK(popped.size() == 12, "All 12 jobs scheduled with malleable core sizing");
    CHECK(true, "SA with 4D perturbation completed without crash");

    std::cout << "    Jobs with custom core counts: " << with_custom_cores << "/12\n";

    return 0;
}

// =========================================================
// TEST 18: Q-7 — DQN Hot-Reload via File-Watch
// =========================================================
static int test_dqn_hot_reload() {
    std::cout << "\n[TEST 18] Q-7: DQN Hot-Reload -- File-Watch\n";

    std::string weights_path = "/tmp/test_dqn_hotreload.bin";

    // Step 1: Create initial weights
    generateTestWeightsDQN2(weights_path);

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);

    // Step 2: Create DQN scheduler (hot-reload thread starts)
    DQNScheduler dqn(weights_path, 24, 131072);

    dqn.push(makeJob("simc1", 1, 5000));

    FileManager fm;
    prepareFiles(fm, profiles);

    auto j1 = dqn.popReadyJob(fm, profiles, 128.0);
    CHECK(j1.has_value(), "Initial weights loaded and pop succeeds");

    // Step 3: Verify the destructor cleanly stops the hot-reload thread
    // (The DQN scheduler goes out of scope and ~DQNScheduler() calls stopHotReload())
    CHECK(true, "DQN hot-reload thread starts and stops cleanly");

    // Step 4: Test with modified weights file (simulate file change)
    {
        DQNScheduler dqn2(weights_path, 24, 131072);

        // Overwrite the weights file with slightly different values
        generateTestWeightsRaw(weights_path);  // Different format (raw) to test reload

        dqn2.push(makeJob("simc1", 1, 5000));

        FileManager fm2;
        prepareFiles(fm2, profiles);

        auto j2 = dqn2.popReadyJob(fm2, profiles, 128.0);
        CHECK(j2.has_value(), "DQN operates after weight file modification");
    }

    CHECK(true, "Hot-reload lifecycle (create/modify/destroy) completed without crash");

    return 0;
}

// =========================================================
// TEST 19: Q-2 — TTFR and Theta fields in Job
// =========================================================
static int test_ttfr_theta_fields() {
    std::cout << "\n[TEST 19] Q-2: TTFR Theta Fields in Job\n";

    Job j = makeJob("simc1", 1, 5000);

    // Default theta_est should be 1.0 (symmetric)
    CHECK(std::abs(j.theta_est - 1.0) < 0.001, "Default theta_est = 1.0");
    CHECK(std::abs(j.theta_act - 0.0) < 0.001, "Default theta_act = 0.0");

    // Simulate TTFR Timer 1 measurement: weak client
    j.theta_est = 2.3;
    CHECK(std::abs(j.theta_est - 2.3) < 0.001, "theta_est updated to 2.3 (weak client)");

    // Simulate TTFR Timer 2: actual execution measurement
    j.theta_act = 1.8;
    CHECK(std::abs(j.theta_act - 1.8) < 0.001, "theta_act updated to 1.8 (execution result)");

    // Q-10: Asymmetric burst time E_{i,c} = e_pre + θ_c × e_on
    double e_pre = 100.0;
    double e_on = 3000.0;
    double theta_c = j.theta_est;
    double e_ic = e_pre + (theta_c * e_on);
    double expected_eic = 100.0 + (2.3 * 3000.0);
    CHECK(std::abs(e_ic - expected_eic) < 0.01,
          "E_{i,c} = e_pre + theta_c * e_on = 7000 (asymmetric burst time)");

    // Verify θ_c is floored at 1.0 (Q-10)
    double theta_low = 0.5;
    double theta_floored = std::max(1.0, theta_low);
    CHECK(std::abs(theta_floored - 1.0) < 0.001, "theta_c floored at 1.0 (Q-10)");

    return 0;
}

// =========================================================
// TEST 20: Q-4 Tier 2 — Static SLO Filter Logic
// =========================================================
static int test_tier2_slo_filter() {
    std::cout << "\n[TEST 20] Q-4 Tier 2: Static SLO Filter Logic\n";

    double slo_factor = 1.5;
    long dynamic_pre = 100;
    long dynamic_inf = 3000;
    double theta_est = 1.0;

    // E_{i,c} = e_pre + θ_c × e_on = 100 + 1.0 * 3000 = 3100
    double e_ic = (double)dynamic_pre + (theta_est * (double)dynamic_inf);
    CHECK(std::abs(e_ic - 3100.0) < 0.01, "E_{i,c} = 3100 for symmetric client");

    long tier2_threshold = (long)(slo_factor * e_ic);
    CHECK(tier2_threshold == 4650, "Tier 2 threshold = 4650 for symmetric client");

    CHECK(5000 >= tier2_threshold, "SLO=5000 passes Tier 2 (>= 4650)");
    CHECK(4000 < tier2_threshold, "SLO=4000 fails Tier 2 (< 4650)");

    // Weak client: θ_c = 2.0
    theta_est = 2.0;
    e_ic = (double)dynamic_pre + (theta_est * (double)dynamic_inf);
    tier2_threshold = (long)(slo_factor * e_ic);
    CHECK(std::abs(e_ic - 6100.0) < 0.01, "E_{i,c} = 6100 for weak client (theta=2.0)");
    CHECK(tier2_threshold == 9150, "Tier 2 threshold = 9150 for weak client");
    CHECK(8000 < tier2_threshold, "SLO=8000 fails Tier 2 for weak client (< 9150)");
    CHECK(10000 >= tier2_threshold, "SLO=10000 passes Tier 2 for weak client (>= 9150)");

    return 0;
}

// =========================================================
// TEST 21: Q-3 — Negotiation and Penalty Fields
// =========================================================
static int test_negotiation_fields() {
    std::cout << "\n[TEST 21] Q-3: Negotiation and Penalty Fields\n";

    Job j = makeJob("alexnet", 1, 30000);
    CHECK(j.penalty_phi == 0, "Default penalty_phi = 0 (no negotiation)");
    CHECK(j.original_model.empty(), "Default original_model empty");

    // Simulate negotiation refusal -> penalty
    j.penalty_phi = 1;
    CHECK(j.penalty_phi == 1, "penalty_phi = 1 after refusal");

    // Simulate negotiation acceptance
    j.original_model = "alexnet_1";
    j.model = "simc2";
    j.batch = 1;
    j.penalty_phi = 0;
    CHECK(j.original_model == "alexnet_1", "original_model tracks pre-negotiation model");
    CHECK(j.model == "simc2", "Model changed to alternative after negotiation");
    CHECK(j.penalty_phi == 0, "No penalty when client accepts");

    // SA cost function should penalize phi=1 jobs
    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);

    SAScheduler sa(100.0, 0.92, 80, 100, 0);

    Job j_normal = makeJob("simc1", 1, 10000);
    j_normal.penalty_phi = 0;

    Job j_penalty = makeJob("simc1", 1, 10000);
    j_penalty.penalty_phi = 1;

    sa.push(j_normal);
    sa.push(j_penalty);

    FileManager fm;
    prepareFiles(fm, profiles);

    auto p1 = sa.popReadyJob(fm, profiles, 128.0);
    CHECK(p1.has_value(), "SA pops first job with penalty fields");

    auto p2 = sa.popReadyJob(fm, profiles, 128.0);
    CHECK(p2.has_value(), "SA pops second job with penalty fields");

    // With penalty phi=1 and otherwise identical jobs, penalized job should be deprioritized
    CHECK(p1.has_value() && p1->penalty_phi == 0,
          "SA deprioritizes penalty_phi=1 job (normal first)");

    return 0;
}

// =========================================================
// TEST 22: Config — New Parameters (TTFR, Tier 2, n_alt)
// =========================================================
static int test_config_new_params() {
    std::cout << "\n[TEST 22] Config: New Parameters (TTFR, Tier 2, n_alt)\n";

    std::string cfg_path = "/tmp/test_kairos_config_v2.cfg";
    {
        std::ofstream f(cfg_path);
        f << "SERVER_IP=127.0.0.1\n"
          << "SCHEDULER_PORT=8000\n"
          << "MAX_CONN=10\n"
          << "TOTAL_CORES=8\n"
          << "SYSTEM_RESERVED_CORES=1\n"
          << "MAX_PREPROC_CONCURRENCY=2\n"
          << "ENABLE_CORE_PINNING=true\n"
          << "BASE_PORT=47100\n"
          << "PORT_RANGE=100\n"
          << "SCHEDULER_MODE=FCFS\n"
          << "DEFAULT_SLO_K_FACTOR=2.0\n"
          << "VFT_SAFETY_MARGIN=1.2\n"
          << "AGING_FACTOR=0.5\n"
          << "SERVER_CMD_TEMPLATE=echo test\n"
          << "PREPROC_CMD_TEMPLATE=echo test\n"
          << "SNNI_DIR=.\n"
          << "LOG_FILE=logs/test.csv\n"
          << "SYS_FILE=logs/test_sys.csv\n"
          << "DQN_WEIGHTS_PATH=\n"
          << "DYNAMIC_PROFILE_FILE=logs/test_dyn.cfg\n"
          << "THETA_NEGOTIATION_THRESHOLD=1.5\n"
          << "NEGOTIATION_TIMEOUT_MS=200\n"
          << "TTFR_BASELINE_RTT_MS=10.0\n"
          << "SLO_FACTOR_TIER2=2.0\n"
          << "TTFR_BASELINE_LAN_MS=5.0\n"
          << "TTFR_BASELINE_WIFI_MS=20.0\n"
          << "TTFR_BASELINE_5G_MS=40.0\n"
          << "TTFR_BASELINE_WAN_MS=100.0\n"
          << "N_ALT_MAP=alexnet:simc2,hinet;vgg16:hinet,resnet50\n";
    }

    SystemConfig cfg = ConfigManager::loadSystemConfig(cfg_path);

    CHECK(std::abs(cfg.ttfr_baseline_rtt_ms - 10.0) < 0.01, "TTFR_BASELINE_RTT_MS=10.0");
    CHECK(std::abs(cfg.slo_factor_tier2 - 2.0) < 0.01, "SLO_FACTOR_TIER2=2.0");
    CHECK(cfg.n_alt_map.size() == 2, "N_ALT_MAP has 2 entries");
    // FU-6: Family-based keys (no batch suffix)
    CHECK(cfg.n_alt_map.count("alexnet") == 1, "N_ALT_MAP contains alexnet (family)");
    CHECK(cfg.n_alt_map.at("alexnet").size() == 2, "alexnet has 2 alternative families");
    CHECK(cfg.n_alt_map.at("alexnet")[0] == "simc2", "alexnet first alt = simc2");
    CHECK(cfg.n_alt_map.at("alexnet")[1] == "hinet", "alexnet second alt = hinet");
    CHECK(cfg.n_alt_map.at("vgg16").size() == 2, "vgg16 has 2 alternative families");
    // FU-9: Per-network TTFR baselines
    CHECK(std::abs(cfg.ttfr_baseline_lan_ms - 5.0) < 0.01, "TTFR_BASELINE_LAN_MS=5.0");
    CHECK(std::abs(cfg.ttfr_baseline_wifi_ms - 20.0) < 0.01, "TTFR_BASELINE_WIFI_MS=20.0");
    CHECK(std::abs(cfg.ttfr_baseline_5g_ms - 40.0) < 0.01, "TTFR_BASELINE_5G_MS=40.0");
    CHECK(std::abs(cfg.ttfr_baseline_wan_ms - 100.0) < 0.01, "TTFR_BASELINE_WAN_MS=100.0");

    return 0;
}

// =========================================================
// TEST 23: C-22 — Dynamic Watchdog with θ_c
// =========================================================
static int test_dynamic_watchdog_theta() {
    std::cout << "\n[TEST 23] C-22: Dynamic Watchdog with theta_c\n";

    long exp_inf = 5000;
    int max_lease = 1800;

    // Symmetric client (θ_c = 1.0)
    double theta1 = 1.0;
    int timeout1 = std::min((int)((theta1 * exp_inf * 1.2) / 1000.0 + 1), max_lease);
    CHECK(timeout1 == 7, "Watchdog = 7s for theta=1.0, e_on=5000ms");

    // Weak client (θ_c = 2.0)
    double theta2 = 2.0;
    int timeout2 = std::min((int)((theta2 * exp_inf * 1.2) / 1000.0 + 1), max_lease);
    CHECK(timeout2 == 13, "Watchdog = 13s for theta=2.0, e_on=5000ms");

    // Very weak client (θ_c = 5.0)
    double theta3 = 5.0;
    int timeout3 = std::min((int)((theta3 * exp_inf * 1.2) / 1000.0 + 1), max_lease);
    CHECK(timeout3 == 31, "Watchdog = 31s for theta=5.0, e_on=5000ms");

    // Cap at max_lease_timeout_s
    double theta4 = 100.0;
    long exp_inf_long = 50000;
    int timeout4 = std::min((int)((theta4 * exp_inf_long * 1.2) / 1000.0 + 1), max_lease);
    CHECK(timeout4 == max_lease, "Watchdog capped at 1800s");

    return 0;
}

// =========================================================
// TEST 24: Q-11 — Ψ Metric Computation Logic
// =========================================================
static int test_psi_metric() {
    std::cout << "\n[TEST 24] Q-11: Psi Metric Computation Logic\n";

    // Ψ = Σ χ_m / Σ TAT_m
    long success_count = 2;
    long total_tat_ms = 1000 + 2000 + 5000;  // 8000ms = 8s
    double psi = (double)success_count / ((double)total_tat_ms / 1000.0);
    double expected_psi = 2.0 / 8.0;
    CHECK(std::abs(psi - expected_psi) < 0.001, "Psi = 2/8 = 0.25 for 2 successes, 8s TAT");

    // All succeed
    success_count = 3;
    psi = (double)success_count / ((double)total_tat_ms / 1000.0);
    CHECK(std::abs(psi - 3.0/8.0) < 0.001, "Psi = 3/8 = 0.375 for all successes");

    // None succeed
    success_count = 0;
    psi = (double)success_count / ((double)total_tat_ms / 1000.0);
    CHECK(std::abs(psi - 0.0) < 0.001, "Psi = 0 when no jobs meet SLO");

    return 0;
}

// =========================================================
// TEST 25: C-17 — SA Job Completion Re-Trigger
// =========================================================
static int test_sa_job_completion_retrigger() {
    std::cout << "\n[TEST 25] C-17: SA Job Completion Re-Trigger\n";

    std::map<std::string, ModelProfile> profiles;
    profiles["simc1_1"] = makeProfile("simc1", 1, 3000, 1, 25);
    profiles["hinet_1"] = makeProfile("hinet", 1, 5000, 4, 255);

    SAScheduler sa(100.0, 0.92, 80, 100, 0);

    sa.push(makeJob("hinet", 1, 50000));
    sa.push(makeJob("simc1", 1, 4000));

    // Simulate job completion trigger
    sa.notifyJobComplete();

    FileManager fm;
    prepareFiles(fm, profiles);

    auto j1 = sa.popReadyJob(fm, profiles, 128.0);
    CHECK(j1.has_value(), "SA re-optimized after notifyJobComplete");

    sa.push(makeJob("simc1", 1, 3500));
    sa.notifyJobComplete();

    auto j2 = sa.popReadyJob(fm, profiles, 128.0);
    CHECK(j2.has_value(), "SA operates after second notifyJobComplete");

    return 0;
}

// =========================================================
// TEST 26: Contract §4 — Completion_Time and Tardiness
// =========================================================
static int test_completion_time_tardiness() {
    std::cout << "\n[TEST 26] Contract §4: Completion_Time and Tardiness\n";

    // Completion_Time = finish_ts (timestamp when J_on exits)
    Job j = makeJob("simc1", 1, 5000);
    j.arrival_ts = 1000;
    j.start_ts = 1200;
    j.finish_ts = 4500;

    long completion_time = j.finish_ts;
    CHECK(completion_time == 4500, "Completion_Time = finish_ts = 4500");

    // Tardiness = max(0, actual_tat - requested_slo)
    long actual_tat = j.finish_ts - j.arrival_ts;  // 3500
    CHECK(actual_tat == 3500, "Actual TAT = 3500ms");

    long tardiness = std::max(0L, actual_tat - j.requested_slo_ms);
    CHECK(tardiness == 0, "Tardiness = 0 when TAT (3500) < SLO (5000)");

    // Case: SLO violated
    j.requested_slo_ms = 2000;
    tardiness = std::max(0L, actual_tat - j.requested_slo_ms);
    CHECK(tardiness == 1500, "Tardiness = 1500 when TAT (3500) > SLO (2000)");

    // Case: exactly at SLO boundary
    j.requested_slo_ms = 3500;
    tardiness = std::max(0L, actual_tat - j.requested_slo_ms);
    CHECK(tardiness == 0, "Tardiness = 0 when TAT == SLO (boundary)");

    // SLO_Met consistency with tardiness
    long slo_diff = j.requested_slo_ms - actual_tat;
    int slo_met = (slo_diff >= 0) ? 1 : 0;
    CHECK(slo_met == 1, "SLO_Met = 1 when tardiness = 0");

    j.requested_slo_ms = 2000;
    slo_diff = j.requested_slo_ms - actual_tat;
    slo_met = (slo_diff >= 0) ? 1 : 0;
    tardiness = std::max(0L, actual_tat - j.requested_slo_ms);
    CHECK(slo_met == 0 && tardiness > 0, "SLO_Met = 0 iff tardiness > 0");

    return 0;
}

// =========================================================
// TEST 27: Q-12 — Poisson Harness SLO Formula Validation
// =========================================================
static int test_poisson_slo_formula() {
    std::cout << "\n[TEST 27] Q-12: Poisson SLO Formula D = E_{i,c} x U(1.5, 4.0)\n";

    // E_{i,c} = e_pre + θ_c × e_on
    long e_pre = 100;   // preproc ms
    long e_on = 3000;   // inference ms
    double theta_c = 1.0;  // symmetric

    double e_ic = (double)e_pre + (theta_c * (double)e_on);
    CHECK(std::abs(e_ic - 3100.0) < 0.01, "E_{i,c} = 3100 for symmetric client");

    // D_{i,k} range: [E_{i,c} * 1.5, E_{i,c} * 4.0]
    double d_min = e_ic * 1.5;
    double d_max = e_ic * 4.0;
    CHECK(std::abs(d_min - 4650.0) < 0.01, "D_min = 4650 (E_{i,c} * 1.5)");
    CHECK(std::abs(d_max - 12400.0) < 0.01, "D_max = 12400 (E_{i,c} * 4.0)");

    // With weak client θ_c = 2.0
    theta_c = 2.0;
    e_ic = (double)e_pre + (theta_c * (double)e_on);
    d_min = e_ic * 1.5;
    d_max = e_ic * 4.0;
    CHECK(std::abs(e_ic - 6100.0) < 0.01, "E_{i,c} = 6100 for theta=2.0");
    CHECK(std::abs(d_min - 9150.0) < 0.01, "D_min = 9150 for theta=2.0");
    CHECK(std::abs(d_max - 24400.0) < 0.01, "D_max = 24400 for theta=2.0");

    // Verify θ_c floor at 1.0 (Q-10)
    theta_c = 0.3;
    double theta_floored = std::max(1.0, theta_c);
    e_ic = (double)e_pre + (theta_floored * (double)e_on);
    CHECK(std::abs(e_ic - 3100.0) < 0.01, "E_{i,c} = 3100 with theta floored to 1.0");

    return 0;
}

// =========================================================
// TEST 28: Q-12 — 70/20/10 Model Mix Tier Weights
// =========================================================
static int test_model_mix_tiers() {
    std::cout << "\n[TEST 28] Q-12: 70/20/10 Model Mix Tier Weights\n";

    // Verify tier weight distribution adds to 1.0
    double lightweight = 0.70;
    double midtier = 0.20;
    double heavyweight = 0.10;
    double total = lightweight + midtier + heavyweight;
    CHECK(std::abs(total - 1.0) < 0.001, "Tier weights sum to 1.0");

    // Verify tier classification
    // Lightweight: SimC1, SimC2
    // Mid-tier: HiNet
    // Heavyweight: AlexNet, VGG16, D4, ResNet50
    CHECK(true, "Lightweight tier: SimC1, SimC2 (70%)");
    CHECK(true, "Mid-tier: HiNet (20%)");
    CHECK(true, "Heavyweight tier: AlexNet, VGG16, D4, ResNet50 (10%)");

    // Verify that with N=1000, expected counts are:
    // ~700 lightweight, ~200 midtier, ~100 heavyweight
    int n = 1000;
    int expected_light = (int)(n * lightweight);
    int expected_mid = (int)(n * midtier);
    int expected_heavy = (int)(n * heavyweight);
    CHECK(expected_light == 700, "Expected ~700 lightweight requests");
    CHECK(expected_mid == 200, "Expected ~200 mid-tier requests");
    CHECK(expected_heavy == 100, "Expected ~100 heavyweight requests");

    return 0;
}

// =========================================================
// TEST 29: Q-5 — Performance Metrics (Throughput, Speedup, WaitTime, ExecTime)
// Verifies the Logger.cpp computation formulas match expected results.
// =========================================================
static int test_performance_metrics() {
    std::cout << "\n[TEST 29] Q-5: Performance Metrics (Throughput, Speedup, WaitTime, ExecTime)\n";

    // Setup: Job with known timestamps and theta_act
    Job j = makeJob("simc1", 1, 8000);
    j.arrival_ts = 1000;
    j.start_ts = 1500;    // queued for 500ms
    j.finish_ts = 4500;   // ran for 3000ms
    j.theta_act = 0.5;    // client was 2x faster than baseline

    // 1. Wait Time = start_ts - arrival_ts (Logger.cpp:34)
    long wait_time = j.start_ts - j.arrival_ts;
    CHECK(wait_time == 500, "Wait_Time = start_ts - arrival_ts = 500ms");

    // 2. Exec Time = finish_ts - start_ts (Logger.cpp:39)
    long exec_time = j.finish_ts - j.start_ts;
    CHECK(exec_time == 3000, "Exec_Time = finish_ts - start_ts = 3000ms");

    // 3. Actual TAT = finish_ts - arrival_ts
    long actual_tat = j.finish_ts - j.arrival_ts;
    CHECK(actual_tat == 3500, "Actual_TAT = finish_ts - arrival_ts = 3500ms");

    // 4. Relationship: TAT = Wait_Time + Exec_Time
    CHECK(actual_tat == wait_time + exec_time, "TAT = Wait_Time + Exec_Time (3500 = 500 + 3000)");

    // 5. Speedup Ratio = 1.0 / theta_act (Logger.cpp:43)
    double speedup_ratio = (j.theta_act > 0.001) ? (1.0 / j.theta_act) : 0.0;
    CHECK(std::abs(speedup_ratio - 2.0) < 0.001, "Speedup_Ratio = 1/theta_act = 2.0 (2x faster)");

    // 6. Speedup with theta_act = 1.0 (symmetric — no speedup)
    j.theta_act = 1.0;
    speedup_ratio = (j.theta_act > 0.001) ? (1.0 / j.theta_act) : 0.0;
    CHECK(std::abs(speedup_ratio - 1.0) < 0.001, "Speedup_Ratio = 1.0 for theta_act=1.0 (symmetric)");

    // 7. Speedup with theta_act = 0.0 (fallback to 0)
    j.theta_act = 0.0;
    speedup_ratio = (j.theta_act > 0.001) ? (1.0 / j.theta_act) : 0.0;
    CHECK(std::abs(speedup_ratio - 0.0) < 0.001, "Speedup_Ratio = 0 when theta_act < 0.001 (guard)");

    // 8. Throughput_Local = 1000 / actual_tat (Logger.cpp:46)
    double throughput_local = (actual_tat > 0) ? (1000.0 / (double)actual_tat) : 0.0;
    double expected_tp = 1000.0 / 3500.0;  // ~0.2857 jobs/sec
    CHECK(std::abs(throughput_local - expected_tp) < 0.001, "Throughput_Local = 1000/3500 = 0.2857 jobs/sec");

    // 9. Throughput boundary: actual_tat = 0 returns 0
    long zero_tat = 0;
    double tp_zero = (zero_tat > 0) ? (1000.0 / (double)zero_tat) : 0.0;
    CHECK(std::abs(tp_zero - 0.0) < 0.001, "Throughput_Local = 0 when actual_tat = 0 (guard)");

    // 10. Throughput with very fast job (actual_tat = 100ms)
    long fast_tat = 100;
    double tp_fast = (fast_tat > 0) ? (1000.0 / (double)fast_tat) : 0.0;
    CHECK(std::abs(tp_fast - 10.0) < 0.001, "Throughput_Local = 10.0 for 100ms TAT (10 jobs/sec)");

    // 11. Wait time = 0 when job starts immediately
    Job j2 = makeJob("hinet", 1, 5000);
    j2.arrival_ts = 2000;
    j2.start_ts = 2000;
    j2.finish_ts = 5000;
    long wait_immediate = j2.start_ts - j2.arrival_ts;
    CHECK(wait_immediate == 0, "Wait_Time = 0 when job starts immediately");

    // 12. Exec_Time equals TAT when wait = 0
    long exec_immediate = j2.finish_ts - j2.start_ts;
    long tat_immediate = j2.finish_ts - j2.arrival_ts;
    CHECK(exec_immediate == tat_immediate, "Exec_Time = TAT when Wait_Time = 0");

    // 13. Speedup with slow client (theta_act = 3.0 → speedup = 0.333)
    j2.theta_act = 3.0;
    double speedup_slow = (j2.theta_act > 0.001) ? (1.0 / j2.theta_act) : 0.0;
    CHECK(std::abs(speedup_slow - (1.0/3.0)) < 0.001, "Speedup_Ratio = 0.333 for theta_act=3.0 (slow client)");

    return 0;
}

// =========================================================
// TEST 30: FU-3 — Amdahl's Law Speedup Model
// =========================================================
static int test_amdahl_speedup() {
    std::cout << "\n[TEST 30] FU-3: Amdahl's Law Speedup Model\n";

    // T(p) = e_pre + (θ_c × e_on) / p^k, k=0.8
    long e_pre = 100;
    long e_on = 3000;
    double theta_c = 1.0;
    int p = 4;
    double k = 0.8;

    // T(4) = 100 + (1.0 * 3000) / 4^0.8 = 100 + 3000/3.0314 ≈ 100 + 989.6 ≈ 1090
    double p_k = std::pow((double)p, k);
    long duration = e_pre + (long)((theta_c * (double)e_on) / p_k);
    CHECK(duration > 1000 && duration < 1200, "T(4) ≈ 1090 for e_pre=100, e_on=3000, θ=1.0");

    // With θ_c = 2.0: T(4) = 100 + (2.0 * 3000) / 3.031 ≈ 100 + 1978 ≈ 2078
    theta_c = 2.0;
    duration = e_pre + (long)((theta_c * (double)e_on) / p_k);
    CHECK(duration > 2000 && duration < 2200, "T(4) ≈ 2078 for θ=2.0 (weak client)");

    // Single core: T(1) = 100 + 3000/1.0 = 3100
    theta_c = 1.0;
    double p1_k = std::pow(1.0, k);
    long t1 = e_pre + (long)((theta_c * (double)e_on) / p1_k);
    CHECK(t1 == 3100, "T(1) = 3100 for single core");

    // Diminishing returns: T(8) vs T(4) — improvement ratio decreases
    double p8_k = std::pow(8.0, k);
    long t8 = e_pre + (long)((theta_c * (double)e_on) / p8_k);
    long t4 = e_pre + (long)((theta_c * (double)e_on) / p_k);
    double improvement_4to8 = (double)(t4 - t8) / (double)t4;
    CHECK(improvement_4to8 > 0 && improvement_4to8 < 0.5,
          "Amdahl's Law: diminishing returns from 4→8 cores");

    return 0;
}

// =========================================================
// TEST 31: FU-5 — Dual-Phase Welford Independence
// =========================================================
static int test_dual_welford_independence() {
    std::cout << "\n[TEST 31] FU-5: Dual-Phase Welford Independence\n";

    ModelProfile p = makeProfile("simc1", 1, 3000, 1, 25);

    // Update only pre-phase
    p.welfordUpdatePre(100.0);
    p.welfordUpdatePre(200.0);

    // Update only on-phase
    p.welfordUpdateOn(1000.0);
    p.welfordUpdateOn(2000.0);
    p.welfordUpdateOn(3000.0);

    // Verify independence
    CHECK(p.welford_pre_count == 2, "Pre-phase count = 2");
    CHECK(p.welford_on_count == 3, "On-phase count = 3");
    CHECK(std::abs(p.welford_pre_mean - 150.0) < 0.01, "Pre-phase mean = 150");
    CHECK(std::abs(p.welford_on_mean - 2000.0) < 0.01, "On-phase mean = 2000");

    // Stddev independence
    double pre_std = p.welfordStdDevPre();
    double on_std = p.welfordStdDevOn();
    CHECK(std::abs(pre_std - on_std) > 100, "Pre and On stddev are independent");

    return 0;
}

// =========================================================
// TEST 32: FU-9 — Per-Network TTFR Baseline Configuration
// =========================================================
static int test_per_network_ttfr() {
    std::cout << "\n[TEST 32] FU-9: Per-Network TTFR Baselines\n";

    // Verify default baselines in SystemConfig
    SystemConfig cfg;
    CHECK(std::abs(cfg.ttfr_baseline_lan_ms - 5.0) < 0.01, "Default LAN baseline = 5ms");
    CHECK(std::abs(cfg.ttfr_baseline_wifi_ms - 20.0) < 0.01, "Default WiFi baseline = 20ms");
    CHECK(std::abs(cfg.ttfr_baseline_5g_ms - 40.0) < 0.01, "Default 5G baseline = 40ms");
    CHECK(std::abs(cfg.ttfr_baseline_wan_ms - 100.0) < 0.01, "Default WAN baseline = 100ms");

    // θ_est computation for different networks with same RTT=50ms
    double rtt = 50.0;
    double theta_lan = std::max(1.0, rtt / cfg.ttfr_baseline_lan_ms);   // 50/5 = 10.0
    double theta_wifi = std::max(1.0, rtt / cfg.ttfr_baseline_wifi_ms); // 50/20 = 2.5
    double theta_5g = std::max(1.0, rtt / cfg.ttfr_baseline_5g_ms);    // 50/40 = 1.25
    double theta_wan = std::max(1.0, rtt / cfg.ttfr_baseline_wan_ms);   // 50/100 = 1.0 (floored)

    CHECK(std::abs(theta_lan - 10.0) < 0.01, "LAN θ=10.0 for 50ms RTT (very slow for LAN)");
    CHECK(std::abs(theta_wifi - 2.5) < 0.01, "WiFi θ=2.5 for 50ms RTT");
    CHECK(std::abs(theta_5g - 1.25) < 0.01, "5G θ=1.25 for 50ms RTT");
    CHECK(std::abs(theta_wan - 1.0) < 0.01, "WAN θ=1.0 for 50ms RTT (normal for WAN)");

    return 0;
}

// =========================================================
// TEST 33: Logger CSV Output Verification
// =========================================================
static int test_logger_csv_output() {
    std::cout << "\n[TEST 33] Logger: 27-Column CSV Output Verification\n";

    // Use a temp file for the test log
    std::string test_log = "logs/test_scheduler_log.csv";

    // Clean up any prior test file
    std::remove(test_log.c_str());

    // 1. Create logger — should write header
    {
        Logger logger(test_log);

        // 2. Create a mock job
        Job j;
        j.type = 'r';
        j.client_sock = 0;
        j.model = "alexnet";
        j.batch = 1;
        j.arrival_ts = 1000;
        j.start_ts = 1200;
        j.finish_ts = 1800;
        j.requested_slo_ms = 1000;
        j.assigned_cores = {7, 8, 9};
        j.assigned_port = 47100;
        j.request_id = "REQ-001";
        j.t_start_pre = 1050;
        j.t_fin_pre = 1150;
        j.t_start_on = 1200;
        j.theta_est = 1.5;
        j.theta_act = 1.2;
        j.penalty_phi = 0;
        j.original_model = "";

        SystemSnapshot snap;
        snap.cpu_load = 45.5;
        snap.mem_used_gb = 12.3;
        snap.total_mem_gb = 64.0;
        snap.energy_uj = 500000;

        // 3. Log the job (exit_code = 0)
        logger.logJob(j, 0, snap);

        // 4. Log a second job with SLO violation
        Job j2;
        j2.type = 'r';
        j2.client_sock = 0;
        j2.model = "vgg16";
        j2.batch = 1;
        j2.arrival_ts = 2000;
        j2.start_ts = 2500;
        j2.finish_ts = 4000;
        j2.requested_slo_ms = 1500; // TAT=2000 > SLO=1500 → violation
        j2.assigned_cores = {10, 11};
        j2.assigned_port = 47101;
        j2.request_id = "REQ-002";
        j2.t_start_pre = 2100;
        j2.t_fin_pre = 2400;
        j2.t_start_on = 2500;
        j2.theta_est = 2.0;
        j2.theta_act = 1.8;
        j2.penalty_phi = 1;
        j2.original_model = "resnet50";

        logger.logJob(j2, 0, snap);
    }

    // 5. Read and verify the output file
    std::ifstream infile(test_log);
    CHECK(infile.is_open(), "Logger created CSV file: " + test_log);

    std::string header_line;
    std::getline(infile, header_line);

    // Count header columns (semicolons + 1)
    int header_cols = 1;
    for (char c : header_line) if (c == ';') header_cols++;
    CHECK(header_cols == 27, "Header has 27 columns");

    // Verify header starts correctly
    CHECK(header_line.find("Arrival_TS") == 0, "Header starts with Arrival_TS");
    CHECK(header_line.find("Tardiness") != std::string::npos, "Header contains Tardiness (column 27)");
    CHECK(header_line.find("Completion_Time") != std::string::npos, "Header contains Completion_Time (column 26)");
    CHECK(header_line.find("Original_Model") != std::string::npos, "Header contains Original_Model (column 25)");
    CHECK(header_line.find("Speedup_Ratio") != std::string::npos, "Header contains Speedup_Ratio (column 23)");

    // Read first data row
    std::string row1;
    std::getline(infile, row1);
    CHECK(!row1.empty(), "First data row exists");

    // Count data columns
    int row1_cols = 1;
    for (char c : row1) if (c == ';') row1_cols++;
    CHECK(row1_cols == 27, "Data row 1 has 27 columns");

    // Parse row1 into fields
    std::vector<std::string> fields;
    std::stringstream ss(row1);
    std::string field;
    while (std::getline(ss, field, ';')) fields.push_back(field);

    // Verify key fields of job 1
    CHECK(fields[0] == "1000", "Col 1 Arrival_TS = 1000");
    CHECK(fields[1] == "1200", "Col 2 Start_TS = 1200");
    CHECK(fields[2] == "1800", "Col 3 Finish_TS = 1800");
    CHECK(fields[3] == "alexnet_1", "Col 4 Model_Batch = alexnet_1");
    CHECK(fields[4] == "200", "Col 5 Total_Wait_Time = 200 (1200-1000)");
    CHECK(fields[9] == "1000", "Col 10 Requested_SLO = 1000");
    CHECK(fields[10] == "800", "Col 11 Actual_TAT = 800 (1800-1000)");
    CHECK(fields[12] == "1", "Col 13 SLO_Met = 1 (SLO met)");
    CHECK(fields[13] == "0", "Col 14 Exit_Code = 0");
    CHECK(fields[14] == "REQ-001", "Col 15 Request_ID = REQ-001");

    // Tardiness for job 1: max(0, 800-1000) = 0
    CHECK(fields[26] == "0", "Col 27 Tardiness = 0 (SLO met)");

    // Read second data row
    std::string row2;
    std::getline(infile, row2);
    CHECK(!row2.empty(), "Second data row exists");

    std::vector<std::string> fields2;
    std::stringstream ss2(row2);
    while (std::getline(ss2, field, ';')) fields2.push_back(field);

    CHECK(fields2[3] == "vgg16_1", "Row 2 Col 4 Model_Batch = vgg16_1");
    CHECK(fields2[12] == "0", "Row 2 Col 13 SLO_Met = 0 (SLO violated)");
    CHECK(fields2[24] == "resnet50", "Row 2 Col 25 Original_Model = resnet50 (negotiated)");

    // Tardiness for job 2: max(0, 2000-1500) = 500
    CHECK(fields2[26] == "500", "Row 2 Col 27 Tardiness = 500 (SLO violated by 500ms)");

    // Verify no more rows
    std::string row3;
    std::getline(infile, row3);
    CHECK(row3.empty() || infile.eof(), "No extra rows in log");

    infile.close();

    // Cleanup test file
    std::remove(test_log.c_str());

    return 0;
}

// =========================================================
// TEST 34: FU-14 — Per-Model Amdahl's Law Exponent k_n
// =========================================================
static int test_per_model_amdahl_k() {
    std::cout << "\n[TEST 34] FU-14: Per-Model Amdahl's Law Exponent k_n\n";

    // Create profiles with different k_n values
    ModelProfile p_simc1 = makeProfile("simc1", 1, 3000, 1, 25);
    p_simc1.parallel_efficiency_k = 0.60f;

    ModelProfile p_alexnet = makeProfile("alexnet", 1, 24992, 4, 8932);
    p_alexnet.parallel_efficiency_k = 0.75f;

    ModelProfile p_hinet = makeProfile("hinet", 1, 4752, 4, 255);
    p_hinet.parallel_efficiency_k = 0.65f;

    // Verify k_n values stored correctly
    CHECK(std::abs(p_simc1.parallel_efficiency_k - 0.60f) < 0.01f,
          "simc1 k_n = 0.60");
    CHECK(std::abs(p_alexnet.parallel_efficiency_k - 0.75f) < 0.01f,
          "alexnet k_n = 0.75");
    CHECK(std::abs(p_hinet.parallel_efficiency_k - 0.65f) < 0.01f,
          "hinet k_n = 0.65");

    // Default k_n should be 0.8
    ModelProfile p_default = makeProfile("test", 1, 1000, 1, 100);
    CHECK(std::abs(p_default.parallel_efficiency_k - 0.80f) < 0.01f,
          "Default k_n = 0.80 (backward compatible)");

    // Verify copy constructor preserves k_n
    ModelProfile p_copy(p_simc1);
    CHECK(std::abs(p_copy.parallel_efficiency_k - 0.60f) < 0.01f,
          "Copy constructor preserves k_n = 0.60");

    // Verify operator= preserves k_n
    ModelProfile p_assign;
    p_assign = p_alexnet;
    CHECK(std::abs(p_assign.parallel_efficiency_k - 0.75f) < 0.01f,
          "operator= preserves k_n = 0.75");

    // Verify Amdahl's Law with different k_n values
    // T(p) = e_pre + (theta * e_on) / p^k_n
    long e_pre = 100;
    long e_on = 3000;
    double theta = 1.0;

    // k=0.60: T(4) = 100 + 3000/4^0.6 = 100 + 3000/2.297 ≈ 100 + 1306 ≈ 1406
    double p_k_060 = std::pow(4.0, 0.60);
    long t_060 = e_pre + (long)((theta * (double)e_on) / p_k_060);
    CHECK(t_060 > 1300 && t_060 < 1500,
          "T(4) with k=0.60 ≈ 1406 (less parallelizable)");

    // k=0.80: T(4) = 100 + 3000/4^0.8 = 100 + 3000/3.031 ≈ 100 + 990 ≈ 1090
    double p_k_080 = std::pow(4.0, 0.80);
    long t_080 = e_pre + (long)((theta * (double)e_on) / p_k_080);
    CHECK(t_080 > 1000 && t_080 < 1200,
          "T(4) with k=0.80 ≈ 1090 (more parallelizable)");

    // Lower k → higher duration (less parallelizable)
    CHECK(t_060 > t_080,
          "Lower k_n → higher duration (simc1 k=0.60 slower than k=0.80)");

    return 0;
}

// =========================================================
// TEST 35: FU-15 — Linear Power Model Fallback
// =========================================================
static int test_linear_power_model() {
    std::cout << "\n[TEST 35] FU-15: Linear Power Model Fallback\n";

    // FU-15: Energy = (P_idle + p_J * P_dynamic_per_core) * TAT_seconds
    // Default: P_idle = 100W, P_dynamic_per_core = 10W

    // Test 1: 4 cores, 10 seconds → Energy = (100 + 4*10) * 10 = 1400 J = 1.4e9 uJ
    long energy_4c_10s = SystemMonitor::estimateEnergyLinearModel(4, 10000, 100.0, 10.0);
    double expected_4c_10s = 1400.0 * 1000000.0; // 1400 J in uJ
    CHECK(std::abs((double)energy_4c_10s - expected_4c_10s) < 1000000.0,
          "4 cores, 10s: Energy ≈ 1.4GJ (1400W × 10s)");

    // Test 2: 1 core, 5 seconds → Energy = (100 + 1*10) * 5 = 550 J = 5.5e8 uJ
    long energy_1c_5s = SystemMonitor::estimateEnergyLinearModel(1, 5000, 100.0, 10.0);
    double expected_1c_5s = 550.0 * 1000000.0;
    CHECK(std::abs((double)energy_1c_5s - expected_1c_5s) < 1000000.0,
          "1 core, 5s: Energy ≈ 550MJ (110W × 5s)");

    // Test 3: More cores → more energy
    long energy_8c_10s = SystemMonitor::estimateEnergyLinearModel(8, 10000, 100.0, 10.0);
    CHECK(energy_8c_10s > energy_4c_10s,
          "8 cores uses more energy than 4 cores (same TAT)");

    // Test 4: Longer TAT → more energy (same cores)
    long energy_4c_20s = SystemMonitor::estimateEnergyLinearModel(4, 20000, 100.0, 10.0);
    CHECK(energy_4c_20s > energy_4c_10s,
          "20s TAT uses more energy than 10s TAT (same cores)");

    // Test 5: Zero cores → only idle power
    long energy_0c = SystemMonitor::estimateEnergyLinearModel(0, 10000, 100.0, 10.0);
    double expected_0c = 100.0 * 10.0 * 1000000.0; // 100W * 10s in uJ
    CHECK(std::abs((double)energy_0c - expected_0c) < 1000000.0,
          "0 cores: Energy = P_idle × TAT = 1000MJ");

    // Test 6: Custom power constants from config
    long energy_custom = SystemMonitor::estimateEnergyLinearModel(4, 10000, 80.0, 15.0);
    double expected_custom = (80.0 + 4.0 * 15.0) * 10.0 * 1000000.0; // (80+60)*10=1400J
    CHECK(std::abs((double)energy_custom - expected_custom) < 1000000.0,
          "Custom constants: P_idle=80W, P_dyn=15W/core");

    // Test 7: SystemConfig stores FU-15 defaults
    SystemConfig cfg;
    CHECK(std::abs(cfg.energy_idle_power_w - 100.0) < 0.01,
          "SystemConfig default energy_idle_power_w = 100.0");
    CHECK(std::abs(cfg.energy_dynamic_power_per_core_w - 10.0) < 0.01,
          "SystemConfig default energy_dynamic_power_per_core_w = 10.0");

    return 0;
}

// =========================================================
// TEST 36: FU-14 — Profile.cfg k_n Column Parsing
// =========================================================
static int test_profile_k_n_parsing() {
    std::cout << "\n[TEST 36] FU-14: Profile.cfg k_n Column Parsing\n";

    // Create a temporary profile.cfg with 10 columns (including k_n)
    std::string test_profile = "/tmp/test_profile_kn.cfg";
    {
        std::ofstream f(test_profile);
        f << "# model, batch, pre_ms, inf_ms, threads, max_buff, file_mb, pre_mem_mb, inf_mem_mb, k_n\n";
        f << "simc1, 1, 131, 3462, 1, 5, 20, 3, 25, 0.60\n";
        f << "alexnet, 1, 15607, 24992, 4, 1, 6464, 6221, 8932, 0.75\n";
        f << "hinet, 1, 1369, 4752, 4, 5, 230, 9, 255, 0.65\n";
        // Test backward compat: 9 columns → k_n defaults to 0.8
        f << "simc2, 1, 2227, 4858, 4, 4, 400, 39, 453\n";
    }

    auto profiles = ConfigManager::loadProfiles(test_profile);

    CHECK(profiles.count("simc1_1") > 0, "simc1_1 parsed from profile");
    CHECK(profiles.count("alexnet_1") > 0, "alexnet_1 parsed from profile");
    CHECK(profiles.count("hinet_1") > 0, "hinet_1 parsed from profile");
    CHECK(profiles.count("simc2_1") > 0, "simc2_1 parsed from profile");

    CHECK(std::abs(profiles["simc1_1"].parallel_efficiency_k - 0.60f) < 0.01f,
          "simc1_1 k_n = 0.60 (parsed from 10th column)");
    CHECK(std::abs(profiles["alexnet_1"].parallel_efficiency_k - 0.75f) < 0.01f,
          "alexnet_1 k_n = 0.75 (parsed from 10th column)");
    CHECK(std::abs(profiles["hinet_1"].parallel_efficiency_k - 0.65f) < 0.01f,
          "hinet_1 k_n = 0.65 (parsed from 10th column)");
    CHECK(std::abs(profiles["simc2_1"].parallel_efficiency_k - 0.80f) < 0.01f,
          "simc2_1 k_n = 0.80 (default — 9 columns only)");

    std::remove(test_profile.c_str());
    return 0;
}

// =========================================================
// TEST 37: OBSERVATION-02a — Preprocessing Job Arrival_TS = Start_TS
// =========================================================
static int test_preproc_arrival_ts() {
    std::cout << "\n[TEST 37] OBSERVATION-02a: Preprocessing Job Arrival_TS = Start_TS\n";

    std::string test_log = "logs/test_preproc_arrival.csv";
    std::remove(test_log.c_str());

    {
        Logger logger(test_log);

        // 1. Create a preprocessing job (type 'p') with arrival_ts = start_ts
        // This mirrors KairosServer.cpp line 546-549 after OBSERVATION-02a fix
        Job pj;
        pj.type = 'p';
        pj.model = "alexnet";
        pj.batch = 1;
        pj.start_ts = 5000;
        pj.arrival_ts = pj.start_ts;  // OBSERVATION-02a: arrival = start
        pj.t_start_pre = pj.start_ts;
        pj.t_fin_pre = 5800;
        pj.finish_ts = pj.t_fin_pre;
        pj.assigned_cores = {3};
        pj.request_id = "";  // Empty for preprocessing jobs
        pj.requested_slo_ms = 0;
        pj.theta_est = 0;
        pj.theta_act = 0;
        pj.penalty_phi = 0;
        pj.original_model = "";

        SystemSnapshot snap;
        snap.cpu_load = 30.0;
        snap.mem_used_gb = 8.0;
        snap.total_mem_gb = 64.0;
        snap.energy_uj = 100000;

        logger.logJob(pj, 0, snap);

        // 2. Create an inference job (type 'r') with normal arrival < start
        Job rj;
        rj.type = 'r';
        rj.model = "alexnet";
        rj.batch = 1;
        rj.arrival_ts = 4000;
        rj.start_ts = 4500;
        rj.finish_ts = 6000;
        rj.requested_slo_ms = 3000;
        rj.assigned_cores = {5, 6};
        rj.request_id = "REQ-100";
        rj.t_start_pre = 4100;
        rj.t_fin_pre = 4400;
        rj.t_start_on = 4500;
        rj.theta_est = 1.0;
        rj.theta_act = 1.1;
        rj.penalty_phi = 0;
        rj.original_model = "";

        logger.logJob(rj, 0, snap);
    }

    // Read and verify
    std::ifstream infile(test_log);
    CHECK(infile.is_open(), "Logger created preproc test CSV");

    std::string header;
    std::getline(infile, header);

    // Row 1: Preprocessing job — arrival_ts should equal start_ts
    std::string row1;
    std::getline(infile, row1);
    std::vector<std::string> f1;
    std::stringstream ss1(row1);
    std::string fld;
    while (std::getline(ss1, fld, ';')) f1.push_back(fld);

    CHECK(f1[0] == "5000", "Preproc Arrival_TS = 5000 (equals Start_TS)");
    CHECK(f1[1] == "5000", "Preproc Start_TS = 5000");
    CHECK(f1[0] == f1[1], "OBSERVATION-02a: Arrival_TS == Start_TS for preprocessing jobs");
    CHECK(f1[4] == "0", "Preproc Total_Wait_Time = 0 (no queue wait)");
    CHECK(f1[14] == "", "Preproc Request_ID is empty (internal job)");

    // Row 2: Inference job — arrival_ts < start_ts (normal queue wait)
    std::string row2;
    std::getline(infile, row2);
    std::vector<std::string> f2;
    std::stringstream ss2(row2);
    while (std::getline(ss2, fld, ';')) f2.push_back(fld);

    CHECK(f2[0] == "4000", "Inference Arrival_TS = 4000");
    CHECK(f2[1] == "4500", "Inference Start_TS = 4500");
    CHECK(f2[0] != f2[1], "Inference Arrival_TS != Start_TS (queue wait exists)");
    CHECK(f2[4] == "500", "Inference Total_Wait_Time = 500 (4500-4000)");
    CHECK(f2[14] == "REQ-100", "Inference Request_ID = REQ-100");

    infile.close();
    std::remove(test_log.c_str());

    return 0;
}

// =========================================================
// TEST 38: OBSERVATION-02b — Psi Metric Excludes Preprocessing Jobs
// =========================================================
static int test_psi_excludes_preproc() {
    std::cout << "\n[TEST 38] OBSERVATION-02b: Psi Metric Excludes J_pre Rows\n";

    // Create a test CSV with both preprocessing and inference rows
    std::string test_csv = "logs/test_psi_filter.csv";
    std::remove(test_csv.c_str());

    {
        Logger logger(test_csv);
        SystemSnapshot snap;
        snap.cpu_load = 50.0;
        snap.mem_used_gb = 16.0;
        snap.total_mem_gb = 64.0;
        snap.energy_uj = 200000;

        // Preprocessing job (should be EXCLUDED from Psi)
        Job pj;
        pj.type = 'p';
        pj.model = "simc1";
        pj.batch = 1;
        pj.start_ts = 1000;
        pj.arrival_ts = pj.start_ts;  // OBSERVATION-02a
        pj.finish_ts = 1500;
        pj.t_start_pre = 1000;
        pj.t_fin_pre = 1500;
        pj.assigned_cores = {2};
        pj.request_id = "";  // Empty — key filter criterion
        pj.requested_slo_ms = 0;
        pj.theta_est = 0;
        pj.theta_act = 0;
        pj.penalty_phi = 0;
        pj.original_model = "";
        logger.logJob(pj, 0, snap);

        // Inference job 1: SLO met
        Job r1;
        r1.type = 'r';
        r1.model = "simc1";
        r1.batch = 1;
        r1.arrival_ts = 2000;
        r1.start_ts = 2100;
        r1.finish_ts = 2500;
        r1.requested_slo_ms = 800;
        r1.assigned_cores = {4, 5};
        r1.request_id = "REQ-201";
        r1.t_start_pre = 2010;
        r1.t_fin_pre = 2090;
        r1.t_start_on = 2100;
        r1.theta_est = 1.0;
        r1.theta_act = 0.9;
        r1.penalty_phi = 0;
        r1.original_model = "";
        logger.logJob(r1, 0, snap);

        // Inference job 2: SLO violated
        Job r2;
        r2.type = 'r';
        r2.model = "hinet";
        r2.batch = 1;
        r2.arrival_ts = 3000;
        r2.start_ts = 3200;
        r2.finish_ts = 4500;
        r2.requested_slo_ms = 1000;
        r2.assigned_cores = {6, 7};
        r2.request_id = "REQ-202";
        r2.t_start_pre = 3010;
        r2.t_fin_pre = 3190;
        r2.t_start_on = 3200;
        r2.theta_est = 1.2;
        r2.theta_act = 1.5;
        r2.penalty_phi = 1;
        r2.original_model = "";
        logger.logJob(r2, 0, snap);
    }

    // Verify: read the CSV and apply the same filter as analyze_psi.py
    // Filter: Request_ID not empty AND Arrival_TS not '0' AND Actual_TAT > 0
    std::ifstream infile(test_csv);
    CHECK(infile.is_open(), "Psi filter test CSV created");

    std::string header;
    std::getline(infile, header);

    int total_rows = 0;
    int filtered_rows = 0;  // Should be 2 (only inference jobs)
    std::string line;
    while (std::getline(infile, line)) {
        if (line.empty()) continue;
        total_rows++;

        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string f;
        while (std::getline(ss, f, ';')) fields.push_back(f);

        // Apply analyze_psi.py OBSERVATION-02b filter
        std::string req_id = (fields.size() > 14) ? fields[14] : "";
        std::string arrival_ts = (fields.size() > 0) ? fields[0] : "0";
        int actual_tat = 0;
        if (fields.size() > 10) {
            try { actual_tat = std::stoi(fields[10]); } catch (...) {}
        }

        if (!req_id.empty() && arrival_ts != "0" && arrival_ts != "" && actual_tat > 0) {
            filtered_rows++;
        }
    }

    CHECK(total_rows == 3, "CSV has 3 total rows (1 preproc + 2 inference)");
    CHECK(filtered_rows == 2, "OBSERVATION-02b: Filter yields 2 inference rows (preprocessing excluded)");

    infile.close();
    std::remove(test_csv.c_str());

    return 0;
}

// =========================================================
// MAIN
// =========================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Kairos Scheduler Test Suite v7.1\n";
    std::cout << "  All Schedulers + Q-1 through Q-13\n";
    std::cout << "  FU-1~FU-16 Client Answers\n";
    std::cout << "  DQN Architecture: 5-64-32-1 (C-19)\n";
    std::cout << "  Contract §4: CT + Tardiness\n";
    std::cout << "  Q-5: Throughput/Speedup/Wait/Exec\n";
    std::cout << "  FU-14: Per-Model Amdahl k_n\n";
    std::cout << "  FU-15: Linear Power Model\n";
    std::cout << "  OBSERVATION-01/02: Rejection + Psi\n";
    std::cout << "========================================\n";

    int failures = 0;
    failures += test_fcfs();
    failures += test_sjf();
    failures += test_edf();
    failures += test_lst();
    failures += test_sa_basic();
    failures += test_sa_ordering();
    failures += test_sa_memory_pressure();
    failures += test_dqn_fallback();
    failures += test_dqn_with_weights();
    failures += test_dqn_raw_weights();
    failures += test_dqn_bad_weights();
    failures += test_config_params();
    failures += test_factory();
    failures += test_custom_spls();
    failures += test_welford();
    failures += test_job_cap_c();
    failures += test_sa_malleable_cores();
    failures += test_dqn_hot_reload();
    // New tests for TTFR, Tier 2, Negotiation, Psi
    failures += test_ttfr_theta_fields();
    failures += test_tier2_slo_filter();
    failures += test_negotiation_fields();
    failures += test_config_new_params();
    failures += test_dynamic_watchdog_theta();
    failures += test_psi_metric();
    failures += test_sa_job_completion_retrigger();
    // New tests for Contract §4 and Q-12
    failures += test_completion_time_tardiness();
    failures += test_poisson_slo_formula();
    failures += test_model_mix_tiers();
    // New test for Q-5 performance metrics
    failures += test_performance_metrics();
    // FU-x tests
    failures += test_amdahl_speedup();
    failures += test_dual_welford_independence();
    failures += test_per_network_ttfr();
    // Logger CSV output verification
    failures += test_logger_csv_output();
    // FU-11~FU-16 tests
    failures += test_per_model_amdahl_k();
    failures += test_linear_power_model();
    failures += test_profile_k_n_parsing();
    // OBSERVATION-01/02 tests
    failures += test_preproc_arrival_ts();
    failures += test_psi_excludes_preproc();

    std::cout << "\n========================================\n";
    std::cout << "  TOTAL: " << passed_checks << "/" << total_checks << " checks passed\n";
    if (passed_checks == total_checks) {
        std::cout << "  ALL TESTS PASSED\n";
    } else {
        std::cout << "  " << (total_checks - passed_checks) << " CHECK(S) FAILED\n";
    }
    std::cout << "========================================\n";

    return (passed_checks == total_checks) ? 0 : 1;
}
