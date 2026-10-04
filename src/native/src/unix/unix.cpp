#include "unix/unix.h"

#include <clocale>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <optional>

#include <poll.h>
#include <signal.h>

namespace dcn::native {
    bool configureTerminal()
    {
        const char * locale_name = ::setlocale(LC_ALL, "");
        if (locale_name == nullptr)
        {
            return false;
        }

        return true;
    }

    std::pair<int, std::string> runProcess(
        const std::string& command,
        std::vector<std::string> args,
        std::optional<std::chrono::milliseconds> timeout)
    {
        const bool has_path_separator = command.find('/') != std::string::npos;
        if (has_path_separator && access(command.c_str(), X_OK) != 0)
        {
            throw std::runtime_error("Command not executable: " + command);
        }

        // Build argv array
        std::vector<char*> argv;
        argv.reserve(args.size() + 2); // command + args + null
        argv.push_back(const_cast<char*>(command.c_str())); // argv[0] = program name

        for (const auto& arg : args)
        {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        int pipefd[2];
        if (pipe(pipefd) == -1)
        {
            spdlog::error("Failed to create pipe");
            throw std::runtime_error("Failed to create pipe");
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            spdlog::error("Failed to fork");
            throw std::runtime_error("Failed to fork");
        }

        if (pid == 0) 
        {
            // Child process
            if (timeout)
            {
                // Its own process group, so the deadline kills everything it starts.
                setpgid(0, 0);
            }
            close(pipefd[0]); // Close read end
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            execvp(command.c_str(), argv.data());

            // If exec fails
            spdlog::error("execvp failed");
            perror("execvp failed");
            _exit(127);
        }

        // Parent process
        if (timeout)
        {
            // Also set here, so the group exists whichever of the two runs first.
            setpgid(pid, pid);
        }
        close(pipefd[1]); // Close write end

        std::string output;
        char buffer[256];

        const auto deadline = timeout
            ? std::optional(std::chrono::steady_clock::now() + *timeout)
            : std::nullopt;
        bool killed = false;

        while (true)
        {
            if (deadline && !killed)
            {
                const long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    *deadline - std::chrono::steady_clock::now()).count();
                pollfd descriptor{.fd = pipefd[0], .events = POLLIN, .revents = 0};
                const int ready = remaining > 0
                    ? poll(&descriptor, 1, static_cast<int>(std::min<long long>(remaining, INT_MAX)))
                    : 0;
                if (ready < 0 && errno == EINTR)
                {
                    continue;
                }
                if (ready == 0)
                {
                    // The child outlived its deadline: stop it and everything it started,
                    // then drain what they wrote.
                    if (kill(-pid, SIGKILL) != 0)
                    {
                        kill(pid, SIGKILL);
                    }
                    killed = true;
                    continue;
                }
            }

            const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
            if (count > 0)
            {
                output.append(buffer, static_cast<std::size_t>(count));
            }
            else if (count == 0 || errno != EINTR)
            {
                break;
            }
        }

        close(pipefd[0]);

        int status;
        if (waitpid(pid, &status, 0) == -1)
        {
            spdlog::error("waitpid failed");
            throw std::runtime_error("waitpid failed");
        }

        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (killed)
        {
            output += "\n[process terminated: deadline exceeded]";
            exit_code = -1;
        }

        return {exit_code, output};
    }
} // namespace dcn::native
