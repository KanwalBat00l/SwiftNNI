#include "Types.hpp"
#include "ProcessRunner.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>
#include <iostream>

CmdResult ProcessRunner::run(const Job& job, 
                             const std::string& work_dir, 
                             int timeout_sec) {
    
    pid_t pid = fork();
    if (pid < 0) return {-1, "fork_failed", false};

    if (pid == 0) { // Child process
        if (!work_dir.empty()) {
            if (chdir(work_dir.c_str()) != 0) _exit(126);
        }

        // Use the threads already assigned to the job struct
        setenv("OMP_NUM_THREADS", std::to_string(job.assigned_threads).c_str(), 1);

        execl("/bin/sh", "sh", "-c", job.cmd.c_str(), (char*)NULL);
        _exit(127);
    } 

    // Parent process
    auto start = std::chrono::steady_clock::now();
    while (true) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid) {
            if (WIFEXITED(status)) return {WEXITSTATUS(status), "ok", false};
            return {-1, "abnormal_exit", false};
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= timeout_sec) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0); 
            return {-2, "timeout", true}; 
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}