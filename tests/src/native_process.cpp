#include "unit-tests.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::tests;

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
