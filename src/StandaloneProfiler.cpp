#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>

namespace fs = std::filesystem;
using namespace std::chrono;

/**
 * @brief Monitor thread function to sample memory of a running PID.
 */
void monitorMemory(pid_t pid, long& out_peak) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    while (kill(pid, 0) == 0) { // While process is alive
        std::ifstream file(path);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("VmHWM:") != std::string::npos) {
                    long peak_kb = 0;
                    sscanf(line.c_str(), "VmHWM: %ld kB", &peak_kb);
                    long current_mb = peak_kb / 1024;
                    if (current_mb > out_peak) out_peak = current_mb;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(milliseconds(100));
    }
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cout << "Usage: ./profiler <model> <batch> <threads> <snni_dir>\n";
        return 1;
    }

    std::string model = argv[1];
    int batch = std::stoi(argv[2]);
    int threads = std::stoi(argv[3]);
    std::string snni_dir = argv[4];
    std::string file_prefix = "prof_" + model + "_" + std::to_string(batch);
    int port = 49999;

    // 1. Hardware Detection: Detect ALL cores Slurm provided
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
    
    std::vector<int> all_available_cores;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) {
            all_available_cores.push_back(i);
        }
    }

    // Determine the specific physical cores to pin the server to
    std::string server_pin_string = "";
    for (int i = 0; i < threads && i < (int)all_available_cores.size(); ++i) {
        server_pin_string += std::to_string(all_available_cores[i]) + (i == threads - 1 ? "" : ",");
    }
    
    std::cout << "--- SAPPIS Standalone Profiler ---" << std::endl;
    std::cout << "[Target] Model: " << model << " | Batch: " << batch << " | Threads: " << threads << std::endl;
    
    std::cout << "[OS Mask] Truly Available Core IDs: ";
    for(int id : all_available_cores) std::cout << id << " "; 
    std::cout << "\n[Pinning] Server will be pinned to: " << server_pin_string << std::endl;

    if (all_available_cores.size() < (size_t)threads) {
        std::cout << "!! WARNING: Requested " << threads << " threads but OS only allows " << all_available_cores.size() << " cores !!" << std::endl;
    }

    // 2. Dealer Phase (Mode 2)
    std::string pre_cmd = "cd " + snni_dir + " && ./build/benchmark-" + model + 
                          " 2 127.0.0.1 " + std::to_string(port) + " " + 
                          std::to_string(batch) + " " + file_prefix;
    
    std::cout << "[Step 1/3] Running Dealer..." << std::endl;
    long pre_peak = 0;
    auto pre_start = high_resolution_clock::now();
    pid_t pre_pid = fork();
    if (pre_pid == 0) {
        execl("/bin/sh", "sh", "-c", pre_cmd.c_str(), (char*)NULL);
        _exit(1);
    }
    std::thread pre_mem_thread(monitorMemory, pre_pid, std::ref(pre_peak));
    waitpid(pre_pid, NULL, 0);
    pre_mem_thread.join();
    long pre_ms = duration_cast<milliseconds>(high_resolution_clock::now() - pre_start).count();

    // Verify file generation
    std::string s_file = snni_dir + "/" + file_prefix + "_server.dat";
    if (!fs::exists(s_file)) {
        std::cerr << "FATAL ERROR: Dealer failed to create " << s_file << std::endl;
        return 1;
    }
    long file_mb = fs::file_size(s_file) / (1024 * 1024);

    // 3. Online Phase (Server Mode 0 + Client Mode 1)
    std::cout << "[Step 2/3] Running Inference..." << std::endl;
    
    // Server is pinned to server_pin_string
    std::string server_cmd = "cd " + snni_dir + " && taskset -c " + server_pin_string + 
                             " ./build/benchmark-" + model + " 0 127.0.0.1 " + 
                             std::to_string(port) + " " + std::to_string(batch) + " " + file_prefix;
    
    // Client runs freely on the remaining cores in the mask
    std::string client_cmd = "cd " + snni_dir + " && ./build/benchmark-" + model + 
                             " 1 127.0.0.1 " + std::to_string(port) + " " + 
                             std::to_string(batch) + " " + file_prefix;

    long inf_peak = 0;
    auto inf_start = high_resolution_clock::now();
    
    pid_t s_pid = fork();
    if (s_pid == 0) {
        execl("/bin/sh", "sh", "-c", server_cmd.c_str(), (char*)NULL);
        _exit(1);
    }
    
    std::this_thread::sleep_for(seconds(2)); // Give server time to initialize

    pid_t c_pid = fork();
    if (c_pid == 0) {
        execl("/bin/sh", "sh", "-c", client_cmd.c_str(), (char*)NULL);
        _exit(1);
    }

    std::thread inf_mem_thread(monitorMemory, s_pid, std::ref(inf_peak));

    waitpid(c_pid, NULL, 0); // Wait for client to finish inference
    kill(s_pid, SIGTERM);   // Client done, shutdown server
    waitpid(s_pid, NULL, 0);
    inf_mem_thread.join();

    long inf_ms = duration_cast<milliseconds>(high_resolution_clock::now() - inf_start).count();

    // 4. Cleanup and Logging
    std::cout << "\n--- Profiling Complete ---" << std::endl;
    std::string result_line = model + ", " + std::to_string(batch) + ", " + 
                              std::to_string(pre_ms) + ", " + std::to_string(inf_ms) + ", " + 
                              std::to_string(threads) + ", 2, " + std::to_string(file_mb) + ", " + 
                              std::to_string(pre_peak) + ", " + std::to_string(inf_peak);
    
    std::cout << "CSV Entry: " << result_line << std::endl;
    
    std::ofstream out("profile.cfg", std::ios::app);
    out << result_line << "\n";
    
    // Physical cleanup
    fs::remove(s_file);
    fs::remove(snni_dir + "/" + file_prefix + "_client.dat");

    return 0;
}