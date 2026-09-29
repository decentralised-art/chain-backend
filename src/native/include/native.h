#pragma once

#if defined(WIN32)
#include "windows/windows.h"
#elif defined(__APPLE__)
#include "mac/mac.h"
#elif defined(__unix__)
#include "unix/unix.h"
#else
#   error "Error, unsupported platform"
#endif

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace dcn::native
{
    /**
     * Configures terminal settings for the current platform.
     * This is primarily used to ensure UTF-8 compatible console output.
     *
     * @return true if terminal configuration succeeded or was not required.
     * @return false if terminal configuration failed.
     */
    bool configureTerminal();

    /**
     * Spawns a new process with the given command.
     * The command must be a valid shell command.
     *
     * @param command The command to execute in the new process
     * @param args The arguments to pass to the command
     * @param timeout When set, a process still running after this long is killed; the
     *                call then returns exit code -1 with whatever output was captured.
     */
    std::pair<int, std::string> runProcess(
        const std::string & command,
        std::vector<std::string> args = {},
        std::optional<std::chrono::milliseconds> timeout = std::nullopt);
}
