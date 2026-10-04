#pragma once

#include <gtest/gtest.h>

#include "decentralised_art.hpp"

using namespace std::chrono_literals;

namespace dcn::tests
{
    // "Suite_Name" of the running test: unique per test, so files named with it are never
    // shared by tests running in parallel processes.
    inline std::string currentTestName()
    {
        const auto * info = ::testing::UnitTest::GetInstance()->current_test_info();
        return info ? std::string(info->test_suite_name()) + "_" + info->name() : std::string("no_test");
    }

    class BaseUnitTest : public testing::Test
    {
        public:
            virtual ~BaseUnitTest() = default;

            static void SetUpTestSuite()
            {
                spdlog::set_level(spdlog::level::debug);
            }

            static void TearDownTestSuite()
            {  
            }
    };

    class UnitTest : public BaseUnitTest
    {
        public:
            UnitTest() = default;
    };

    class StressTest : public BaseUnitTest
    {
        public:
            StressTest() = default;
    };
}
