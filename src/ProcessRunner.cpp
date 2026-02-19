#include "Types.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>

class ProcessRunner {
public:
    /**
     * @brief Runs a command based on Job details.
     * @param job The job containing the command and assigned threads.
     * @param snni_dir The working directory from config.
     * @param timeout_sec Calculated dynamic timeout.
     */
    static CmdResult run(const Job& job, const std::string& snni_dir, int timeout_sec) {
        pid_t pid = fork();
        if (pid < 0) return {-1, "fork_failed", false};

        if (pid == 0) { // Child
            if (chdir(snni_dir.c_str()) != 0) _exit(126);
            
            // Set threads based on the job's assigned count
            setenv("OMP_NUM_THREADS", std::to_string(job.assigned_threads).c_str(), 1);

            execl("/bin/sh", "sh", "-c", job.cmd.c_str(), (char*)NULL);
            _exit(127);
        }

        // Parent (with 50ms polling)
        auto start = std::chrono::steady_clock::now();
        while (true) {
            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                if (WIFEXITED(status)) return {WEXITSTATUS(status), "ok", false};
                return {-1, "abnormal_exit", false};
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count() >= timeout_sec) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                return {-2, "timeout", true};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
};