#include "unit-tests.hpp"
#include "fake_solc.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifndef DECENTRALISED_ART_TEST_BINARY_DIR
    #error "DECENTRALISED_ART_TEST_BINARY_DIR is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_SOLC_PATH
    #error "DECENTRALISED_ART_TEST_SOLC_PATH is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_PT_PATH
    #error "DECENTRALISED_ART_TEST_PT_PATH is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_SOLC_CACHE
    #error "DECENTRALISED_ART_TEST_SOLC_CACHE is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_REAL_SOLC_PATH
    #error "DECENTRALISED_ART_TEST_REAL_SOLC_PATH is not defined"
#endif

using namespace dcn;
using namespace dcn::tests;

// Tests of the caching solc stand-in the unit tests compile through (tests/tools/solc_cache.cpp).
namespace
{
    constexpr const char * CACHE_DIR_VARIABLE = "DECENTRALISED_ART_TEST_SOLC_CACHE_DIR";
    constexpr const char * REAL_SOLC_VARIABLE = "DECENTRALISED_ART_TEST_SOLC_CACHE_REAL_SOLC";

    template<class AwaitableT>
    auto runAwaitable(asio::io_context & io_context, AwaitableT awaitable)
    {
        auto future = asio::co_spawn(io_context, std::move(awaitable), asio::use_future);
        io_context.restart();
        io_context.run();
        return future.get();
    }

    void setEnvironment(const char * name, const std::string & value)
    {
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        if(value.empty()) unsetenv(name);
        else setenv(name, value.c_str(), 1);
#endif
    }

    // Sets a variable for the solc processes this test starts, restoring it after.
    class ScopedEnvironment
    {
        public:
            ScopedEnvironment(const char * name, const std::filesystem::path & value)
            :   _name(name)
            {
                if(const char * previous = std::getenv(name)) _previous = previous;
                setEnvironment(name, value.string());
            }

            ~ScopedEnvironment()
            {
                setEnvironment(_name, _previous.value_or(std::string{}));
            }

        private:
            const char * _name;
            std::optional<std::string> _previous;
    };

    const std::string HELPER_V1 = R"(// SPDX-License-Identifier: GPL-3.0
pragma solidity >=0.8.2 <0.9.0;

library CacheHelper {
    function value() internal pure returns (uint256) { return 1; }
}
)";

    const std::string HELPER_V2 = R"(// SPDX-License-Identifier: GPL-3.0
pragma solidity >=0.8.2 <0.9.0;

library CacheHelper {
    function value() internal pure returns (uint256) { return 2; }
}
)";

    const std::string PROBE = R"(// SPDX-License-Identifier: GPL-3.0
pragma solidity >=0.8.2 <0.9.0;

import "CacheHelper.sol";

contract CacheProbe {
    function value() external pure returns (uint256) { return CacheHelper.value(); }
}
)";

    // Written over a cached output file: output carrying it came from the cache, not from solc.
    const std::string MARKER = "fe";

    void writeFile(const std::filesystem::path & path, const std::string & text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    std::string readFile(const std::filesystem::path & path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    // Every published entry, across all keys.
    std::vector<std::filesystem::path> cacheEntries(const std::filesystem::path & cache_root)
    {
        std::vector<std::filesystem::path> entries;
        std::error_code ec;
        for(const auto & key : std::filesystem::directory_iterator(cache_root, ec))
        {
            if(!key.is_directory() || key.path().filename().string().starts_with("staging-")) continue;
            for(const auto & entry : std::filesystem::directory_iterator(key.path(), ec))
            {
                if(entry.is_directory()) entries.push_back(entry.path());
            }
        }
        return entries;
    }

    struct SolcCacheEnv
    {
        std::filesystem::path root =
            std::filesystem::path(DECENTRALISED_ART_TEST_BINARY_DIR) / "tests" / "solc_cache_tests" / currentTestName();
        std::filesystem::path cache_root = root / "cache";
        std::filesystem::path work_dir = root / "work";
        ScopedEnvironment scoped_cache_dir{CACHE_DIR_VARIABLE, cache_root};

        // Never run: the EVM deploys no PT, so the only solc runs are the ones a test asks for.
        asio::io_context idle_io_context;
        asio::io_context io_context;
        evm::EVM evm_instance{idle_io_context, EVMC_SHANGHAI, DECENTRALISED_ART_TEST_SOLC_PATH, DECENTRALISED_ART_TEST_PT_PATH};

        SolcCacheEnv()
        {
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(work_dir);
        }

        ~SolcCacheEnv()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        std::expected<evm::CompiledContract, evm::CompileError> compileProbe()
        {
            return runAwaitable(io_context, evm_instance.compileSource("CacheProbe.sol", PROBE, "CacheProbe", work_dir));
        }
    };

    class SolcCacheTest : public UnitTest
    {
        protected:
            void SetUp() override
            {
                if(!DECENTRALISED_ART_TEST_SOLC_CACHE)
                {
                    GTEST_SKIP() << "tests run the real solc (DECENTRALISED_ART_TEST_SOLC_CACHE_DIR is empty)";
                }
            }
    };
}

TEST_F(SolcCacheTest, CompileSourceReusesOutputOnlyWhileEverySourceIsUnchanged)
{
    SolcCacheEnv env;
    writeFile(env.work_dir / "CacheHelper.sol", HELPER_V1);

    const auto first = env.compileProbe();
    ASSERT_TRUE(first.has_value()) << first.error().message;
    const auto entries = cacheEntries(env.cache_root);
    ASSERT_EQ(entries.size(), 1u);

    writeFile(entries[0] / "CacheProbe.bin-runtime", MARKER);
    const auto cached = env.compileProbe();
    ASSERT_TRUE(cached.has_value()) << cached.error().message;
    EXPECT_EQ(cached->runtime_code, std::vector<std::uint8_t>{0xfe});
    EXPECT_EQ(cached->creation_code, first->creation_code);
    EXPECT_EQ(cached->sources, first->sources);

    // An imported source changed: the entry no longer stands for a fresh run. It stays, next
    // to the new one, since another compilation may be reading it.
    writeFile(env.work_dir / "CacheHelper.sol", HELPER_V2);
    const auto recompiled = env.compileProbe();
    ASSERT_TRUE(recompiled.has_value()) << recompiled.error().message;
    EXPECT_NE(recompiled->runtime_code, std::vector<std::uint8_t>{0xfe});
    EXPECT_NE(recompiled->runtime_code, first->runtime_code);
    EXPECT_NE(recompiled->sources, first->sources);
    EXPECT_EQ(cacheEntries(env.cache_root).size(), 2u);

    writeFile(env.work_dir / "CacheHelper.sol", HELPER_V1);
    const auto restored = env.compileProbe();
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_EQ(restored->runtime_code, std::vector<std::uint8_t>{0xfe});
    EXPECT_EQ(cacheEntries(env.cache_root).size(), 2u);
}

TEST_F(SolcCacheTest, CompileFileReusesTheWholeOutputInAnyOutputDirectory)
{
    SolcCacheEnv env;
    writeFile(env.work_dir / "CacheHelper.sol", HELPER_V1);
    writeFile(env.work_dir / "CacheProbe.sol", PROBE);
    const auto compile_into = [&env](const std::filesystem::path & out_dir)
    {
        return runAwaitable(
            env.io_context,
            env.evm_instance.compile(env.work_dir / "CacheProbe.sol", out_dir, env.work_dir));
    };

    ASSERT_TRUE(compile_into(env.root / "out_a"));
    const auto entries = cacheEntries(env.cache_root);
    ASSERT_EQ(entries.size(), 1u);

    writeFile(entries[0] / "CacheProbe.bin", MARKER);
    ASSERT_TRUE(compile_into(env.root / "out_b"));
    EXPECT_EQ(readFile(env.root / "out_b" / "CacheProbe.bin"), MARKER);
    EXPECT_NE(readFile(env.root / "out_a" / "CacheProbe.bin"), MARKER);
    EXPECT_TRUE(std::filesystem::exists(env.root / "out_b" / "CacheHelper.abi"));
}

TEST_F(SolcCacheTest, KeepsNoFailedCompilation)
{
    SolcCacheEnv env;

    const auto rejected = runAwaitable(
        env.io_context,
        env.evm_instance.compileSource("Broken.sol", "contract Broken {", "Broken", env.work_dir));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind, evm::CompileError::Kind::REJECTED);
    EXPECT_TRUE(cacheEntries(env.cache_root).empty());
}

TEST_F(SolcCacheTest, ForwardsToTheRealSolcWithoutACacheDirectory)
{
    SolcCacheEnv env;
    const ScopedEnvironment no_cache_dir{CACHE_DIR_VARIABLE, std::filesystem::path{}};
    writeFile(env.work_dir / "CacheHelper.sol", HELPER_V1);

    const auto compiled = env.compileProbe();
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;
    EXPECT_FALSE(compiled->runtime_code.empty());
    EXPECT_FALSE(env.evm_instance.solcVersion().empty());
    EXPECT_FALSE(std::filesystem::exists(env.cache_root));
}

TEST_F(SolcCacheTest, ReportsExactlyWhatTheRealSolcReports)
{
    SolcCacheEnv env;

    for(const std::vector<std::string> & args : {
            std::vector<std::string>{"--version"},
            std::vector<std::string>{"--no-such-option"}})
    {
        const auto real = native::runProcess(DECENTRALISED_ART_TEST_REAL_SOLC_PATH, args);
        const auto stand_in = native::runProcess(DECENTRALISED_ART_TEST_SOLC_PATH, args);
        EXPECT_EQ(stand_in.first, real.first) << args.front();
        EXPECT_EQ(stand_in.second, real.second) << args.front();
    }
}

TEST_F(SolcCacheTest, FailsLikeTheRealSolcOnAnImportMoreThanOneRootHolds)
{
    SolcCacheEnv env;
    const auto base = env.work_dir / "base";
    const auto include = env.work_dir / "include";
    std::filesystem::create_directories(base);
    std::filesystem::create_directories(include);
    writeFile(base / "CacheHelper.sol", HELPER_V1);
    writeFile(base / "CacheProbe.sol", PROBE);
    const auto compile_into = [&](const std::filesystem::path & out_dir)
    {
        return runAwaitable(env.io_context, env.evm_instance.compile(base / "CacheProbe.sol", out_dir, base, include));
    };

    ASSERT_TRUE(compile_into(env.root / "out_a"));
    ASSERT_EQ(cacheEntries(env.cache_root).size(), 1u);

    writeFile(include / "CacheHelper.sol", HELPER_V1);
    EXPECT_FALSE(compile_into(env.root / "out_b"));
}

TEST_F(SolcCacheTest, AStalledCompilerEndsWithTheStandInAtItsDeadline)
{
    SolcCacheEnv env;
    const auto started = env.root / "started";
    const ScopedEnvironment stalled_solc{REAL_SOLC_VARIABLE, writeFakeSolc(env.root, started, std::chrono::seconds(30))};

    const auto begin = std::chrono::steady_clock::now();
    const auto [exit_code, output] = native::runProcess(DECENTRALISED_ART_TEST_SOLC_PATH, {"compile"}, std::chrono::seconds(3));

    EXPECT_EQ(exit_code, -1) << output;
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(15));
    EXPECT_TRUE(std::filesystem::exists(started));
}
