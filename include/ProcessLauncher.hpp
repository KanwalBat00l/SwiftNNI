#ifndef PROCESS_LAUNCHER_HPP
#define PROCESS_LAUNCHER_HPP

#include <string>

class ProcessLauncher {
public:
    /**
     * @brief Runs a command with a hard timeout.
     * @return Exit code, -1 for fork error, -2 for timeout.
     */
    static int runShellCommand(const std::string& cmd, int timeout_sec);
};

#endif