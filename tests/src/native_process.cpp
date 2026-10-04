#include "unit-tests.hpp"
#include "fake_solc.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::tests;

#ifndef DECENTRALISED_ART_TEST_BINARY_DIR
    #error "DECENTRALISED_ART_TEST_BINARY_DIR is not defined"
#endif

TEST_F(UnitTest, Native_RunProcess_KillsAProcessThatOutlivesItsTimeout)
{
#if defined(_WIN32)
    const std::string command = "ping";
    const std::vector<std::string> args{"-n", "30", "127.0.0.1"};
#else
    const std::string command = "sleep";
    const std::vector<std::string> args{"30"};
#endif

    const auto started = std::chrono::steady_clock::now();
    const auto [exit_code, output] = native::runProcess(command, args, std::chrono::milliseconds(300));

    EXPECT_EQ(exit_code, -1);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
}

TEST_F(UnitTest, Native_RunProcess_ReturnsTheExitCodeOfAProcessThatFinishesInTime)
{
#if defined(_WIN32)
    const auto [exit_code, output] = native::runProcess("cmd", {"/c", "exit", "3"}, std::chrono::seconds(10));
#else
    const auto [exit_code, output] = native::runProcess("sh", {"-c", "exit 3"}, std::chrono::seconds(10));
#endif

    EXPECT_EQ(exit_code, 3);
}

TEST_F(UnitTest, Native_RunProcess_KillsEverythingAProcessStartedAtItsTimeout)
{
    const std::filesystem::path dir =
        std::filesystem::path(DECENTRALISED_ART_TEST_BINARY_DIR) / "tests" / "native_process" / currentTestName();
    std::filesystem::remove_all(dir);
    // The script stalls in a grandchild that holds the output pipe: only killing that too
    // ends the run before the stall does.
    const auto script = writeFakeSolc(dir, dir / "started", std::chrono::seconds(30));

    const auto started = std::chrono::steady_clock::now();
    const auto [exit_code, output] = native::runProcess(script.string(), {"compile"}, std::chrono::seconds(2));

    EXPECT_EQ(exit_code, -1) << output;
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(15));
    EXPECT_TRUE(std::filesystem::exists(dir / "started"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
