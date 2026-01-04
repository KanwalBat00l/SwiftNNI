#include "ProcessLauncher.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>
#include <iostream>

int ProcessLauncher::runShellCommand(const std::string& cmd, int timeout_sec) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) { // Child process
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    } else { // Parent process
        auto start = std::chrono::steady_clock::now();
        int status;
        
        while (true) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                if (WIFEXITED(status)) return WEXITSTATUS(status);
                return -1;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            
            if (elapsed > timeout_sec) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0); // Cleanup
                return -2; 
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}