#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono;

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
    int port = 49999; // Arbitrary profiling port

    std::cout << "--- SwiftNNI Standalone Profiler ---" << std::endl;
    
    // 1. Dealer Phase (Mode 2)
    std::string pre_cmd = "cd " + snni_dir + " && ./build/benchmark-" + model + 
                          " 2 127.0.0.1 " + std::to_string(port) + " " + 
                          std::to_string(batch) + " " + file_prefix + ".vmfb";
    
    std::cout << "[1/2] Pre-processing..." << std::endl;
    auto pre_start = high_resolution_clock::now();
    pid_t pre_pid = fork();
    if (pre_pid == 0) {
        execl("/bin/sh", "sh", "-c", pre_cmd.c_str(), (char*)NULL);
        _exit(1);
    }
    waitpid(pre_pid, NULL, 0);
    long pre_ms = duration_cast<milliseconds>(high_resolution_clock::now() - pre_start).count();

    // 2. Inference Phase (Server Mode 0 + Client Mode 1)
    std::cout << "[2/2] Running Inference (Threads: " << threads << ")..." << std::endl;
    
    std::string server_cmd = "export OMP_NUM_THREADS=" + std::to_string(threads) + " && cd " + snni_dir + 
                             " && ./build/benchmark-" + model + " 0 127.0.0.1 " + 
                             std::to_string(port) + " " + std::to_string(batch) + " " + file_prefix + ".vmfb";
    
    std::string client_cmd = "cd " + snni_dir + " && ./build/benchmark-" + model + 
                             " 1 127.0.0.1 " + std::to_string(port) + " " + 
                             std::to_string(batch) + " " + file_prefix + ".vmfb";

    auto inf_start = high_resolution_clock::now();
    pid_t s_pid = fork();
    if (s_pid == 0) {
        execl("/bin/sh", "sh", "-c", server_cmd.c_str(), (char*)NULL);
        _exit(1);
    }
    
    std::this_thread::sleep_for(seconds(2)); // Warm up

    pid_t c_pid = fork();
    if (c_pid == 0) {
        execl("/bin/sh", "sh", "-c", client_cmd.c_str(), (char*)NULL);
        _exit(1);
    }

    waitpid(c_pid, NULL, 0); // Wait for client
    kill(s_pid, SIGTERM);
    waitpid(s_pid, NULL, 0);
    long inf_ms = duration_cast<milliseconds>(high_resolution_clock::now() - inf_start).count();

    // 3. Output to profile.cfg format
    std::string result = model + ", " + std::to_string(batch) + ", " + 
                         std::to_string(pre_ms) + ", " + std::to_string(inf_ms) + ", " + 
                         std::to_string(threads);
    
    std::cout << "\nCSV Entry for profile.cfg:\n" << result << std::endl;
    
    std::ofstream out("profile.cfg", std::ios::app);
    out << result << "\n";
    
    // Cleanup
    fs::remove(snni_dir + "/" + file_prefix + ".vmfb");
    return 0;
}