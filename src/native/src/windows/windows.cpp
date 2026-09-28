#include "windows/windows.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace dcn::native{
    bool configureTerminal()
    {
        bool success = true;

        if (!SetConsoleOutputCP(CP_UTF8))
        {
            success = false;
        }

        if (!SetConsoleCP(CP_UTF8))
        {
            success = false;
        }

        const auto enable_vt = [](DWORD std_handle) -> bool
        {
            const HANDLE handle = GetStdHandle(std_handle);
            if (handle == NULL || handle == INVALID_HANDLE_VALUE)
            {
                return true;
            }

            DWORD mode = 0;
            if (!GetConsoleMode(handle, &mode))
            {
                // Not a terminal (e.g. redirected output), nothing to configure.
                return true;
            }

            if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
            {
                return true;
            }

            return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        };

        success = success && enable_vt(STD_OUTPUT_HANDLE);
        success = success && enable_vt(STD_ERROR_HANDLE);

        return success;
    }

    std::pair<int, std::string> runProcess(
        const std::string& command,
        std::vector<std::string> args,
        std::optional<std::chrono::milliseconds> timeout)
    {
        std::ostringstream oss;

        // Quote the command if it contains spaces
        if (command.find(' ') != std::string::npos)
            oss << "\"" << command << "\"";
        else
            oss << command;

        // Append each argument, quoting if needed
        for (const auto& arg : args)
        {
            oss << ' ';
            if (arg.find(' ') != std::string::npos)
                oss << "\"" << arg << "\"";
            else
                oss << arg;
        }

        const std::string& full_cmd = oss.str();

        std::vector<char> cmd_line(full_cmd.size() + 1, '\0');
        strcpy_s(cmd_line.data(), cmd_line.size(), full_cmd.c_str());

        // Create pipe
        HANDLE read_pipe = NULL, write_pipe = NULL;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
            throw std::runtime_error("Failed to create pipe");

        if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("Failed to configure pipe");

        // Prepare startup info
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        ZeroMemory(&pi, sizeof(pi));
        si.cb = sizeof(si);
        si.hStdOutput = write_pipe;
        si.hStdError  = write_pipe; // Capture stderr too
        si.dwFlags |= STARTF_USESTDHANDLES;

        if (!CreateProcessA(
            NULL,         // No module name (use command line)
            cmd_line.data(), // Command line
            NULL,         // Process handle not inheritable
            NULL,         // Thread handle not inheritable
            TRUE,        // Set handle inheritance to FALSE
            0,            // No creation flags
            NULL,         // Use parent's environment block
            NULL,         // Use parent's starting directory 
            &si,          // Pointer to STARTUPINFO structure
            &pi)          // Pointer to PROCESS_INFORMATION structure
        )
        {
            CloseHandle(write_pipe);
            CloseHandle(read_pipe);
            spdlog::error("CreateProcess failed: {}", GetLastError());
            throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError()));
        }

        CloseHandle(write_pipe);

        // Terminates a child that outlives its deadline; reading the pipe then ends.
        // ponytail: only the direct child is killed, which is all solc needs. A tool that
        // starts subprocesses would keep the pipe open past the deadline; put the child in
        // a job object if one ever does.
        std::atomic<bool> timed_out{false};
        std::thread watchdog;
        if (timeout)
        {
            const DWORD timeout_ms = static_cast<DWORD>(std::clamp<long long>(timeout->count(), 0, 0xFFFFFFFELL));
            watchdog = std::thread([process = pi.hProcess, timeout_ms, &timed_out]()
            {
                if (WaitForSingleObject(process, timeout_ms) == WAIT_TIMEOUT)
                {
                    timed_out = true;
                    TerminateProcess(process, 1);
                }
            });
        }

        // 6. Read output
        std::string output;
        char buffer[256];
        DWORD bytes_read;

        while (ReadFile(read_pipe, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            output += buffer;
        }

        // Wait until child process exits
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (watchdog.joinable())
        {
            watchdog.join();
        }

        // Read the return code
        DWORD exit_code;
        if (!GetExitCodeProcess(pi.hProcess, &exit_code))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            throw std::runtime_error("Failed to get process exit code");
        }

        if (exit_code > static_cast<DWORD>(std::numeric_limits<int>::max())) {
            throw std::overflow_error("DWORD value exceeds int range");
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(read_pipe);

        if (timed_out)
        {
            output += "\n[process terminated: deadline exceeded]";
            return {-1, output};
        }

        return {static_cast<int>(exit_code), output};
    }
}
