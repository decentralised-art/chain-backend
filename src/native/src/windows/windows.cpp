#include "windows/windows.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace dcn::native{
    void appendQuotedArgument(std::string & command_line, const std::string & arg)
    {
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        {
            command_line += arg;
            return;
        }

        // The CRT's argv rules: backslashes are literal unless they precede a quote, where
        // each pair yields one backslash and an odd one escapes the quote.
        command_line += '"';
        std::size_t backslashes = 0;
        for (const char c : arg)
        {
            if (c == '\\')
            {
                ++backslashes;
                continue;
            }
            command_line.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
            command_line += c;
            backslashes = 0;
        }
        command_line.append(backslashes * 2, '\\');
        command_line += '"';
    }

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
        std::string full_cmd;

        // The program name is read up to the closing quote, without escapes.
        if (command.find_first_of(" \t") != std::string::npos)
            full_cmd += "\"" + command + "\"";
        else
            full_cmd += command;

        for (const auto& arg : args)
        {
            full_cmd += ' ';
            appendQuotedArgument(full_cmd, arg);
        }

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

        // With a deadline the child runs in a job of its own, so the deadline terminates
        // everything it starts too, including descendants holding the pipe open. It starts
        // suspended until it is in the job, so nothing escapes it.
        HANDLE job = timeout ? CreateJobObjectA(NULL, NULL) : NULL;
        const DWORD creation_flags = job ? CREATE_SUSPENDED : 0;

        if (!CreateProcessA(
            NULL,         // No module name (use command line)
            cmd_line.data(), // Command line
            NULL,         // Process handle not inheritable
            NULL,         // Thread handle not inheritable
            TRUE,        // Set handle inheritance to FALSE
            creation_flags,
            NULL,         // Use parent's environment block
            NULL,         // Use parent's starting directory 
            &si,          // Pointer to STARTUPINFO structure
            &pi)          // Pointer to PROCESS_INFORMATION structure
        )
        {
            CloseHandle(write_pipe);
            CloseHandle(read_pipe);
            if (job) CloseHandle(job);
            spdlog::error("CreateProcess failed: {}", GetLastError());
            throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError()));
        }

        CloseHandle(write_pipe);

        if (job && !AssignProcessToJobObject(job, pi.hProcess))
        {
            // Then the deadline can stop only the child itself.
            CloseHandle(job);
            job = NULL;
        }
        if (creation_flags & CREATE_SUSPENDED)
        {
            ResumeThread(pi.hThread);
        }

        // Terminates a run that outlives its deadline, the child or a descendant still holding
        // the pipe; reading the pipe then ends.
        std::atomic<bool> timed_out{false};
        const HANDLE finished = timeout ? CreateEventA(NULL, TRUE, FALSE, NULL) : NULL;
        std::thread watchdog;
        if (timeout)
        {
            const DWORD timeout_ms = static_cast<DWORD>(std::clamp<long long>(timeout->count(), 0, 0xFFFFFFFELL));
            watchdog = std::thread([process = pi.hProcess, finished, job, timeout_ms, &timed_out]()
            {
                if (WaitForSingleObject(finished ? finished : process, timeout_ms) == WAIT_TIMEOUT)
                {
                    timed_out = true;
                    if (!job || !TerminateJobObject(job, 1))
                    {
                        TerminateProcess(process, 1);
                    }
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
        if (finished)
        {
            SetEvent(finished);
        }
        if (watchdog.joinable())
        {
            watchdog.join();
        }
        if (finished)
        {
            CloseHandle(finished);
        }
        if (job)
        {
            CloseHandle(job);
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
