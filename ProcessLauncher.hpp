#pragma once
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>

class ProcessLauncher {
public:
    static int runShellCommand(const std::string& cmd, int timeout_sec) {
        pid_t pid = fork();
        if (pid < 0) return -1;

        if (pid == 0) { // Child
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
            _exit(127);
        } else { // Parent
            auto start = std::chrono::steady_clock::now();
            int status;
            
            while (true) {
                // Check if process finished
                pid_t result = waitpid(pid, &status, WNOHANG);
                
                if (result == pid) { // Process exited
                    if (WIFEXITED(status)) return WEXITSTATUS(status);
                    return -1;
                }

                // Check for timeout
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
                
                if (elapsed > timeout_sec) {
                    std::cerr << "[Launcher] Timeout reached (" << timeout_sec << "s). Killing process " << pid << std::endl;
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0); // Reap zombie
                    return -2; // Special code for timeout
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
};