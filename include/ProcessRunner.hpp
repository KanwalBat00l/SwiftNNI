#ifndef PROCESS_LAUNCHER_HPP
#define PROCESS_LAUNCHER_HPP

#include <string>

class ProcessRunner {
public:
    /**
     * @brief Runs a command with a hard timeout.
     * @return Exit code, -1 for fork error, -2 for timeout.
     */
     static CmdResult run(const Job& job, const std::string& snni_dir, int timeout_sec);
};

#endif