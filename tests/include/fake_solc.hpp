#pragma once

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace dcn::tests
{
    // Writes a stand-in for solc into `dir` and returns its path. It answers `--version` like
    // solc. Any other run creates `started`, stalls for `stall` in a process of its own (a
    // grandchild of whoever starts the script), then creates a file `late` in the directory
    // passed with `-o` and fails: a compilation still busy, and then still writing, long
    // after it began.
    inline std::filesystem::path writeFakeSolc(
        const std::filesystem::path & dir,
        const std::filesystem::path & started,
        std::chrono::seconds stall)
    {
        std::filesystem::create_directories(dir);
#if defined(_WIN32)
        const std::filesystem::path script = dir / "fake_solc.cmd";
        std::ofstream(script, std::ios::binary | std::ios::trunc) << std::format(
            "@echo off\r\n"
            "if \"%~1\"==\"--version\" (echo Version: 0.8.30+commit.00000000& exit /b 0)\r\n"
            "type nul > \"{}\"\r\n"
            "ping -n {} 127.0.0.1 > nul\r\n"
            ":next\r\n"
            "if \"%~1\"==\"\" exit /b 1\r\n"
            "if \"%~1\"==\"-o\" (mkdir \"%~2\" 2> nul & type nul > \"%~2\\late\")\r\n"
            "shift\r\n"
            "goto next\r\n",
            started.string(), stall.count() + 1);
#else
        const std::filesystem::path script = dir / "fake_solc.sh";
        std::ofstream(script, std::ios::binary | std::ios::trunc) << std::format(
            "#!/bin/sh\n"
            "if [ \"$1\" = \"--version\" ]; then echo 'Version: 0.8.30+commit.00000000'; exit 0; fi\n"
            ": > '{}'\n"
            "sleep {}\n"
            "while [ $# -gt 0 ]; do\n"
            "  if [ \"$1\" = \"-o\" ]; then mkdir -p \"$2\"; : > \"$2/late\"; fi\n"
            "  shift\n"
            "done\n"
            "exit 1\n",
            started.string(), stall.count());
        std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
#endif
        return script;
    }
}
