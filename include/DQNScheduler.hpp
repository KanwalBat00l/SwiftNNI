#pragma once
#include "IScheduler.hpp"
#include "SystemMonitor.hpp"
#include <vector>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <chrono>
#include <iostream>
#include <atomic>
#include <thread>
#include <sys/stat.h>

/**
 * DQN Scheduler — Deep Q-Network priority scoring (C-19).
 *
 * Architecture (matches Developer_Contract.pdf §C-19):
 *   Input(5) → Linear(5,64) + ReLU → Linear(64,32) + ReLU → Linear(32,1)
 *
 * Binary weight file format — DQN2 (C-20):
 *   [4B magic "DQN2"]
 *   [4B uint32 input_dim][4B h1][4B h2][4B output]
 *   [4B uint32 architecture_hash]   — FNV-1a of "5-64-32-1"
 *   [4B uint32 crc32]               — CRC32 of raw weight bytes
 *   [W1: 64*5 float32][b1: 64 float32]
 *   [W2: 32*64 float32][b2: 32 float32]
 *   [W3: 1*32  float32][b3: 1  float32]
 *
 * If weights are not loaded, falls back to LST (Least Slack Time) (C-15).
 *
 * Q-7: Hot-Reload — File-watch every 10s, atomic swap between scheduling cycles.
 *       Optional for Phase 1. Enabled when weights_path is non-empty.
 */
class DQNScheduler : public IScheduler {
public:
    static constexpr int INPUT_DIM  = 5;
    static constexpr int HIDDEN1    = 64;   // C-19: 64 (was 16)
    static constexpr int HIDDEN2    = 32;   // C-19: 32 (was 8)
    static constexpr int OUTPUT_DIM = 1;

    // Total raw weight floats: (5*64+64) + (64*32+32) + (32*1+1) = 384+2080+33 = 2497
    static constexpr size_t WEIGHT_FLOATS = (INPUT_DIM * HIDDEN1 + HIDDEN1) +
                                            (HIDDEN1 * HIDDEN2 + HIDDEN2) +
                                            (HIDDEN2 * OUTPUT_DIM + OUTPUT_DIM);
    static constexpr size_t WEIGHT_BYTES = WEIGHT_FLOATS * sizeof(float); // 9988
    static constexpr size_t DQN2_HEADER_BYTES = 28; // 4 magic + 4*4 dims + 4 arch_hash + 4 crc32
    static constexpr size_t DQN1_HEADER_BYTES = 20; // Legacy: 4 magic + 4*4 dims

    // Legacy DQN1 architecture constants (for backward compatibility detection)
    static constexpr int DQN1_H1 = 16;
    static constexpr int DQN1_H2 = 8;
    static constexpr size_t DQN1_WEIGHT_FLOATS = (INPUT_DIM * DQN1_H1 + DQN1_H1) +
                                                  (DQN1_H1 * DQN1_H2 + DQN1_H2) +
                                                  (DQN1_H2 * OUTPUT_DIM + OUTPUT_DIM);
    static constexpr size_t DQN1_WEIGHT_BYTES = DQN1_WEIGHT_FLOATS * sizeof(float); // 964

    // Q-7: Hot-reload check interval
    static constexpr int HOT_RELOAD_INTERVAL_S = 10;

    DQNScheduler(const std::string& weights_path = "",
                 int total_cores = 0, int total_memory_mb = 0)
        : weightsLoaded(false),
          normCores(total_cores > 0 ? total_cores : 24),
          normMemMb(total_memory_mb > 0 ? total_memory_mb : 0),
          weightsPath(weights_path),
          lastModTime(0),
          hotReloadRunning(false) {
        std::memset(&W, 0, sizeof(W));
        if (!weights_path.empty()) {
            loadWeights(weights_path);
            // Q-7: Record initial mtime and start hot-reload watcher
            lastModTime = getFileMtime(weights_path);
            startHotReload();
        }
    }

    ~DQNScheduler() {
        stopHotReload();
    }

protected:
    void sortQueue(std::map<std::string, ModelProfile>& profiles, double total_mem_gb) override {
        long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
        SystemSnapshot snap = SystemMonitor::takeSnapshot();

        // Config-driven normalization (C-11): use config TOTAL_MEMORY_MB or auto-detect
        double norm_mem_total = (normMemMb > 0) ? (double)normMemMb / 1024.0 : total_mem_gb;
        if (norm_mem_total <= 0) norm_mem_total = snap.total_mem_gb;

        queue.sort([&](const Job& a, const Job& b) {
            double scoreA = forwardPass(a, profiles.at(a.model + "_" + std::to_string(a.batch)),
                                        snap, norm_mem_total, now);
            double scoreB = forwardPass(b, profiles.at(b.model + "_" + std::to_string(b.batch)),
                                        snap, norm_mem_total, now);
            return scoreA > scoreB; // Higher score = higher priority
        });
    }

private:
    struct Weights {
        float W1[HIDDEN1][INPUT_DIM];  // 64x5
        float b1[HIDDEN1];             // 64
        float W2[HIDDEN2][HIDDEN1];    // 32x64
        float b2[HIDDEN2];             // 32
        float W3[OUTPUT_DIM][HIDDEN2]; // 1x32
        float b3[OUTPUT_DIM];          // 1
    } W;

    bool weightsLoaded;
    int normCores;    // C-11: config-driven normalization denominator
    int normMemMb;    // C-11: config-driven memory normalization

    // Q-7: Hot-reload state
    std::string weightsPath;
    time_t lastModTime;
    std::atomic<bool> hotReloadRunning;
    std::thread hotReloadThread;

    static time_t getFileMtime(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return st.st_mtime;
        }
        return 0;
    }

    void startHotReload() {
        if (weightsPath.empty()) return;
        hotReloadRunning = true;
        hotReloadThread = std::thread([this]() {
            while (hotReloadRunning) {
                std::this_thread::sleep_for(std::chrono::seconds(HOT_RELOAD_INTERVAL_S));
                if (!hotReloadRunning) break;

                time_t current_mtime = getFileMtime(weightsPath);
                if (current_mtime > 0 && current_mtime != lastModTime) {
                    std::cerr << "[DQN] Hot-reload: weight file changed, reloading...\n";

                    // Load into a temporary buffer first (atomic swap)
                    Weights newW;
                    std::memset(&newW, 0, sizeof(newW));
                    bool ok = loadWeightsInto(weightsPath, newW);
                    if (ok) {
                        // Atomic swap: copy new weights over current
                        std::lock_guard<std::mutex> lock(mtx);
                        std::memcpy(&W, &newW, sizeof(W));
                        weightsLoaded = true;
                        lastModTime = current_mtime;
                        std::cerr << "[DQN] Hot-reload: weights updated successfully.\n";
                    } else {
                        std::cerr << "[DQN] Hot-reload: reload failed, keeping current weights.\n";
                    }
                }
            }
        });
    }

    void stopHotReload() {
        hotReloadRunning = false;
        if (hotReloadThread.joinable()) {
            hotReloadThread.join();
        }
    }

    /**
     * Q-7: Load weights into a provided Weights struct (for atomic swap).
     * Returns true on success.
     */
    bool loadWeightsInto(const std::string& path, Weights& target) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;

        auto file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (file_size == (std::streampos)(DQN2_HEADER_BYTES + WEIGHT_BYTES)) {
            char magic[4];
            file.read(magic, 4);
            if (std::strncmp(magic, "DQN2", 4) != 0) return false;

            uint32_t dims[4];
            file.read(reinterpret_cast<char*>(dims), 16);
            if (dims[0] != (uint32_t)INPUT_DIM || dims[1] != (uint32_t)HIDDEN1 ||
                dims[2] != (uint32_t)HIDDEN2 || dims[3] != (uint32_t)OUTPUT_DIM) return false;

            uint32_t arch_hash, stored_crc;
            file.read(reinterpret_cast<char*>(&arch_hash), 4);
            file.read(reinterpret_cast<char*>(&stored_crc), 4);

            std::vector<char> weight_buf(WEIGHT_BYTES);
            file.read(weight_buf.data(), WEIGHT_BYTES);

            uint32_t computed_crc = computeCRC32(weight_buf.data(), WEIGHT_BYTES);
            if (computed_crc != stored_crc) return false;

            parseRawWeightsInto(weight_buf.data(), target);
            return true;

        } else if (file_size == (std::streampos)WEIGHT_BYTES) {
            file.read(reinterpret_cast<char*>(target.W1), sizeof(target.W1));
            file.read(reinterpret_cast<char*>(target.b1), sizeof(target.b1));
            file.read(reinterpret_cast<char*>(target.W2), sizeof(target.W2));
            file.read(reinterpret_cast<char*>(target.b2), sizeof(target.b2));
            file.read(reinterpret_cast<char*>(target.W3), sizeof(target.W3));
            file.read(reinterpret_cast<char*>(target.b3), sizeof(target.b3));
            return true;
        }

        return false;
    }

    void parseRawWeightsInto(const char* buf, Weights& target) {
        size_t off = 0;
        std::memcpy(target.W1, buf + off, sizeof(target.W1)); off += sizeof(target.W1);
        std::memcpy(target.b1, buf + off, sizeof(target.b1)); off += sizeof(target.b1);
        std::memcpy(target.W2, buf + off, sizeof(target.W2)); off += sizeof(target.W2);
        std::memcpy(target.b2, buf + off, sizeof(target.b2)); off += sizeof(target.b2);
        std::memcpy(target.W3, buf + off, sizeof(target.W3)); off += sizeof(target.W3);
        std::memcpy(target.b3, buf + off, sizeof(target.b3));
    }

    // CRC32 computation (C-20)
    static uint32_t crc32Table(uint32_t idx) {
        uint32_t crc = idx;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
        return crc;
    }

    static uint32_t computeCRC32(const void* data, size_t len) {
        const uint8_t* buf = static_cast<const uint8_t*>(data);
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; ++i) {
            crc = (crc >> 8) ^ crc32Table((crc ^ buf[i]) & 0xFF);
        }
        return crc ^ 0xFFFFFFFF;
    }

    /**
     * Load binary weights exported by train_dqn.py.
     * Supports:
     *   - DQN2 header (28 + 9988 bytes): "DQN2" + dims + arch_hash + crc32 + weights
     *   - Raw (9988 bytes): raw float32 weights only (no header)
     *   - Detects legacy DQN1 (16-8-1) and warns incompatible
     */
    void loadWeights(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "[DQN] Warning: Cannot open weights file: " << path << "\n";
            std::cerr << "[DQN] Falling back to LST (Least Slack Time) heuristic.\n";
            return;
        }

        auto file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (file_size == (std::streampos)(DQN2_HEADER_BYTES + WEIGHT_BYTES)) {
            // DQN2 format with architecture hash and CRC32
            char magic[4];
            file.read(magic, 4);
            if (std::strncmp(magic, "DQN2", 4) != 0) {
                std::cerr << "[DQN] Error: Invalid magic bytes in " << path
                          << " (expected DQN2)\n";
                return;
            }

            uint32_t dims[4];
            file.read(reinterpret_cast<char*>(dims), 16);
            if (dims[0] != (uint32_t)INPUT_DIM || dims[1] != (uint32_t)HIDDEN1 ||
                dims[2] != (uint32_t)HIDDEN2 || dims[3] != (uint32_t)OUTPUT_DIM) {
                std::cerr << "[DQN] Error: Architecture mismatch. Expected "
                          << INPUT_DIM << "-" << HIDDEN1 << "-" << HIDDEN2 << "-" << OUTPUT_DIM
                          << " but got " << dims[0] << "-" << dims[1] << "-" << dims[2] << "-" << dims[3] << "\n";
                return;
            }

            uint32_t arch_hash, stored_crc;
            file.read(reinterpret_cast<char*>(&arch_hash), 4);
            file.read(reinterpret_cast<char*>(&stored_crc), 4);

            // Read weights into buffer for CRC verification
            std::vector<char> weight_buf(WEIGHT_BYTES);
            file.read(weight_buf.data(), WEIGHT_BYTES);

            uint32_t computed_crc = computeCRC32(weight_buf.data(), WEIGHT_BYTES);
            if (computed_crc != stored_crc) {
                std::cerr << "[DQN] Error: CRC32 mismatch. File may be corrupted.\n";
                std::cerr << "[DQN]   Stored CRC: 0x" << std::hex << stored_crc
                          << " Computed: 0x" << computed_crc << std::dec << "\n";
                return;
            }

            // Parse raw weights from buffer
            parseRawWeightsFromBuffer(weight_buf.data());

        } else if (file_size == (std::streampos)WEIGHT_BYTES) {
            // Raw format (no header) — new architecture
            std::cerr << "[DQN] Loading raw weight format (no header, 64-32-1).\n";
            readRawWeights(file);

        } else if (file_size == (std::streampos)(DQN1_HEADER_BYTES + DQN1_WEIGHT_BYTES) ||
                   file_size == (std::streampos)DQN1_WEIGHT_BYTES) {
            // Legacy DQN1 format (16-8-1) — incompatible architecture
            std::cerr << "[DQN] Warning: DQN1 weight file detected (16-8-1 architecture).\n";
            std::cerr << "[DQN] Current architecture requires 64-32-1. Falling back to LST.\n";
            return;

        } else {
            std::cerr << "[DQN] Error: Unexpected file size " << file_size
                      << " bytes. Expected " << (DQN2_HEADER_BYTES + WEIGHT_BYTES)
                      << " (DQN2) or " << WEIGHT_BYTES << " (raw).\n";
            return;
        }

        weightsLoaded = true;
        std::cerr << "[DQN] Weights loaded successfully from " << path
                  << " (" << WEIGHT_FLOATS << " parameters, 64-32-1 architecture).\n";
    }

    void readRawWeights(std::ifstream& file) {
        file.read(reinterpret_cast<char*>(W.W1), sizeof(W.W1));
        file.read(reinterpret_cast<char*>(W.b1), sizeof(W.b1));
        file.read(reinterpret_cast<char*>(W.W2), sizeof(W.W2));
        file.read(reinterpret_cast<char*>(W.b2), sizeof(W.b2));
        file.read(reinterpret_cast<char*>(W.W3), sizeof(W.W3));
        file.read(reinterpret_cast<char*>(W.b3), sizeof(W.b3));
    }

    void parseRawWeightsFromBuffer(const char* buf) {
        size_t off = 0;
        std::memcpy(W.W1, buf + off, sizeof(W.W1)); off += sizeof(W.W1);
        std::memcpy(W.b1, buf + off, sizeof(W.b1)); off += sizeof(W.b1);
        std::memcpy(W.W2, buf + off, sizeof(W.W2)); off += sizeof(W.W2);
        std::memcpy(W.b2, buf + off, sizeof(W.b2)); off += sizeof(W.b2);
        std::memcpy(W.W3, buf + off, sizeof(W.W3)); off += sizeof(W.W3);
        std::memcpy(W.b3, buf + off, sizeof(W.b3));
    }

    /**
     * Neural network forward pass: Input(5) → 64 ReLU → 32 ReLU → 1
     *
     * Feature vector (must match train_dqn.py, C-11 normalization):
     *   [0] slack_sec  = (arrival_ts + requested_slo_ms - now) / 1000
     *   [1] norm_cpu   = cpu_load / TOTAL_CORES (config-driven)
     *   [2] norm_mem   = mem_used_gb / total_mem_gb
     *   [3] core_req   = profile.threads / TOTAL_CORES (config-driven)
     *   [4] mem_req    = profile.inf_mem_mb / (total_mem_gb * 1024)
     *
     * Returns priority score (higher = higher priority).
     * Falls back to -slack (LST) if weights are not loaded (C-15).
     */
    double forwardPass(const Job& j, const ModelProfile& p,
                       const SystemSnapshot& sys, double total_mem_gb, long now) {
        float slack_sec  = (float)(j.arrival_ts + j.requested_slo_ms - now) / 1000.0f;

        if (!weightsLoaded) {
            // LST fallback (C-15): lower slack = higher priority
            return (double)(-slack_sec);
        }

        // Config-driven normalization (C-11)
        float norm_cpu  = (float)sys.cpu_load / (float)normCores;
        float norm_mem  = (total_mem_gb > 0) ? (float)(sys.mem_used_gb / total_mem_gb) : 0.0f;
        float core_req  = (float)p.threads / (float)normCores;
        float mem_req   = (total_mem_gb > 0) ? (float)p.inf_mem_mb / (float)(total_mem_gb * 1024.0) : 0.0f;

        float input[INPUT_DIM] = { slack_sec, norm_cpu, norm_mem, core_req, mem_req };

        // Layer 1: h1 = ReLU(W1 * input + b1)
        float h1[HIDDEN1];
        for (int i = 0; i < HIDDEN1; ++i) {
            float sum = W.b1[i];
            for (int k = 0; k < INPUT_DIM; ++k) {
                sum += W.W1[i][k] * input[k];
            }
            h1[i] = (sum > 0.0f) ? sum : 0.0f; // ReLU
        }

        // Layer 2: h2 = ReLU(W2 * h1 + b2)
        float h2[HIDDEN2];
        for (int i = 0; i < HIDDEN2; ++i) {
            float sum = W.b2[i];
            for (int k = 0; k < HIDDEN1; ++k) {
                sum += W.W2[i][k] * h1[k];
            }
            h2[i] = (sum > 0.0f) ? sum : 0.0f; // ReLU
        }

        // Layer 3: output = W3 * h2 + b3 (no activation)
        float output = W.b3[0];
        for (int k = 0; k < HIDDEN2; ++k) {
            output += W.W3[0][k] * h2[k];
        }

        return (double)output;
    }
};
