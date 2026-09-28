#include "unit-tests.hpp"

#include <cstring>
#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <ranges>
#include <tuple>

#ifndef DECENTRALISED_ART_TEST_BINARY_DIR
    #error "DECENTRALISED_ART_TEST_BINARY_DIR is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_SOLC_PATH
    #error "DECENTRALISED_ART_TEST_SOLC_PATH is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_PT_PATH
    #error "DECENTRALISED_ART_TEST_PT_PATH is not defined"
#endif

using namespace dcn;
using namespace dcn::tests;

namespace
{
    std::filesystem::path buildPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_BINARY_DIR);
    }

    std::filesystem::path solcPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_SOLC_PATH);
    }

    std::filesystem::path ptPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_PT_PATH);
    }

    chain::Address expectedGenesisAddress()
    {
        chain::Address genesis_address{};
        std::memcpy(genesis_address.bytes + (20 - 7), "genesis", 7);
        return genesis_address;
    }

    chain::Address makeAddressFromSuffix(const char* suffix)
    {
        chain::Address address{};
        const std::size_t suffix_len = std::strlen(suffix);
        if(suffix_len <= 20)
        {
            std::memcpy(address.bytes + (20 - suffix_len), suffix, suffix_len);
        }
        return address;
    }

    bool isZeroAddress(const chain::Address & address)
    {
        return std::ranges::all_of(address.bytes, [](std::uint8_t b) { return b == 0; });
    }

    template<class AwaitableT>
    auto runAwaitable(asio::io_context & io_context, AwaitableT awaitable)
    {
        auto future = asio::co_spawn(io_context, std::move(awaitable), asio::use_future);
        io_context.restart();
        io_context.run();
        return future.get();
    }

    std::vector<std::uint8_t> makeUpgradeToAndCallInput(const chain::Address & new_implementation)
    {
        std::vector<std::uint8_t> input;

        const auto selector = chain::constructSelector("upgradeToAndCall(address,bytes)");
        input.insert(input.end(), selector.begin(), selector.end());

        const auto implementation_arg = evm::encodeAsArg(new_implementation);
        input.insert(input.end(), implementation_arg.begin(), implementation_arg.end());

        // dynamic bytes data offset (0x40, counted from start of args section)
        std::vector<std::uint8_t> bytes_offset(32, 0);
        bytes_offset[31] = 0x40;
        input.insert(input.end(), bytes_offset.begin(), bytes_offset.end());

        // empty bytes payload
        std::vector<std::uint8_t> empty_bytes_len(32, 0);
        input.insert(input.end(), empty_bytes_len.begin(), empty_bytes_len.end());

        return input;
    }

    std::vector<std::uint8_t> makeRunnerGenInput(
        const std::string & connector_name,
        std::uint32_t particles_count,
        const std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> & dynamic_ri)
    {
        std::vector<std::uint8_t> input_data;

        const auto selector = chain::constructSelector("gen(string,uint32,(uint32,uint32,uint32)[])");
        input_data.insert(input_data.end(), selector.begin(), selector.end());

        std::vector<std::uint8_t> offset_to_string(32, 0);
        offset_to_string[31] = 0x60;
        input_data.insert(input_data.end(), offset_to_string.begin(), offset_to_string.end());

        const auto particles_count_arg = evm::encodeAsArg(particles_count);
        input_data.insert(input_data.end(), particles_count_arg.begin(), particles_count_arg.end());

        const auto connector_name_arg = evm::encodeAsArg(connector_name);

        std::vector<std::uint8_t> offset_to_tuple(32, 0);
        const std::size_t tuple_offset = 0x60 + connector_name_arg.size();
        for(std::size_t i = 0; i < sizeof(std::size_t); ++i)
        {
            offset_to_tuple[31 - i] = static_cast<std::uint8_t>((tuple_offset >> (8 * i)) & 0xFFu);
        }
        input_data.insert(input_data.end(), offset_to_tuple.begin(), offset_to_tuple.end());

        input_data.insert(input_data.end(), connector_name_arg.begin(), connector_name_arg.end());

        const auto dynamic_ri_arg = evm::encodeAsArg(dynamic_ri);
        input_data.insert(input_data.end(), dynamic_ri_arg.begin(), dynamic_ri_arg.end());

        return input_data;
    }

    std::filesystem::path makeProxyStoragePath(const std::string & test_name)
    {
        return buildPath() / "tests" / "proxy_upgrade_storage" / test_name;
    }

    bool prepareProxyStorageDirectories(const std::filesystem::path & storage_path)
    {
        std::error_code ec;
        std::filesystem::remove_all(storage_path, ec);
        if(ec)
        {
            spdlog::error("Failed to cleanup test storage '{}': {}", storage_path.string(), ec.message());
            return false;
        }

        static const std::array<std::filesystem::path, 3> build_dirs{
            std::filesystem::path("connectors") / "build",
            std::filesystem::path("transformations") / "build",
            std::filesystem::path("conditions") / "build",
        };

        for(const auto & build_dir : build_dirs)
        {
            ec.clear();
            std::filesystem::create_directories(storage_path / build_dir, ec);
            if(ec)
            {
                spdlog::error("Failed to create storage directory '{}': {}", (storage_path / build_dir).string(), ec.message());
                return false;
            }
        }

        return true;
    }

    std::uint64_t decodeUint256Low64(const std::vector<std::uint8_t> & bytes)
    {
        if(bytes.size() < 32)
        {
            throw std::runtime_error("Invalid ABI data: expected at least 32 bytes");
        }

        std::uint64_t value = 0;
        for(std::size_t i = 24; i < 32; ++i)
        {
            value = (value << 8) | bytes[i];
        }

        return value;
    }

    struct ProxyTestEnv
    {
        asio::io_context io_context{};
        evm::EVM evm_instance;
        chain::Address genesis_address;

        ProxyTestEnv()
            : evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath()),
              genesis_address(expectedGenesisAddress())
        {
            io_context.run();
        }
    };

    struct ScopedLogLevel
    {
        explicit ScopedLogLevel(spdlog::level::level_enum level)
            : previous_level(spdlog::get_level())
        {
            spdlog::set_level(level);
        }

        ~ScopedLogLevel()
        {
            spdlog::set_level(previous_level);
        }

        spdlog::level::level_enum previous_level;
    };

    std::filesystem::path writeRunnerV2Source()
    {
        const std::filesystem::path source_dir = buildPath() / "tests" / "proxy_upgrade_sources";
        std::filesystem::create_directories(source_dir);

        const std::filesystem::path source_path = source_dir / "RunnerV2.sol";

        std::ofstream source_file(source_path, std::ios::out | std::ios::trunc);
        source_file << R"(// SPDX-License-Identifier: GPL-3.0
pragma solidity >=0.8.2 <0.9.0;

import "runner/Runner.sol";

contract RunnerV2 is Runner {
    function version() external pure returns (uint256) {
        return 2;
    }
}
)";
        source_file.close();

        return source_path;
    }

    std::expected<chain::Address, std::string> deployRunnerV2(ProxyTestEnv & env)
    {
        const auto source_path = writeRunnerV2Source();
        const std::filesystem::path out_dir = buildPath() / "tests" / "proxy_upgrade_build";
        std::filesystem::create_directories(out_dir);

        const bool compile_ok = runAwaitable(
            env.io_context,
            env.evm_instance.compile(
                source_path,
                out_dir,
                env.evm_instance.getPTPath() / "contracts",
                env.evm_instance.getPTPath() / "node_modules"));

        if(!compile_ok)
        {
            return std::unexpected(std::format("Failed to compile {}", source_path.string()));
        }

        const auto deploy_result = runAwaitable(
            env.io_context,
            env.evm_instance.deploy(
                out_dir / "RunnerV2.bin",
                env.genesis_address,
                {},
                evm::DEFAULT_GAS_LIMIT,
                0));

        if(!deploy_result)
        {
            return std::unexpected(std::format("Failed to deploy RunnerV2 implementation: {}", deploy_result.error().kind));
        }

        return deploy_result.value();
    }

    void expectParticlesEqual(const std::vector<Particles> & lhs, const std::vector<Particles> & rhs)
    {
        ASSERT_EQ(lhs.size(), rhs.size());
        for(std::size_t i = 0; i < lhs.size(); ++i)
        {
            EXPECT_EQ(lhs[i].path(), rhs[i].path());
            ASSERT_EQ(lhs[i].data_size(), rhs[i].data_size());
            for(int j = 0; j < lhs[i].data_size(); ++j)
            {
                EXPECT_EQ(lhs[i].data(j), rhs[i].data(j));
            }
        }
    }
}

TEST_F(UnitTest, PT_ProxyUpgrade_RunnerUpgradeRequiresOwnerAndSwapsImplementation)
{
    const ScopedLogLevel scoped_log_level(spdlog::level::warn);

    ProxyTestEnv env;

    const auto runner_proxy = env.evm_instance.getRunnerAddress();
    ASSERT_FALSE(isZeroAddress(runner_proxy));

    const auto runner_v2_result = deployRunnerV2(env);
    ASSERT_TRUE(runner_v2_result.has_value()) << runner_v2_result.error();
    const auto runner_v2_implementation = *runner_v2_result;

    // `version()` does not exist before upgrade
    std::vector<std::uint8_t> version_input = chain::constructSelector("version()");
    {
        // This revert is expected: V1 does not expose `version()`.
        const ScopedLogLevel suppress_expected_revert_logs(spdlog::level::critical);
        const auto version_before_upgrade = runAwaitable(
            env.io_context,
            env.evm_instance.execute(
                env.genesis_address,
                runner_proxy,
                version_input,
                evm::DEFAULT_GAS_LIMIT,
                0));
        EXPECT_FALSE(version_before_upgrade.has_value());
    }

    const auto upgrade_input = makeUpgradeToAndCallInput(runner_v2_implementation);

    const auto attacker = makeAddressFromSuffix("attacker");
    runAwaitable(env.io_context, env.evm_instance.addAccount(attacker, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(env.io_context, env.evm_instance.setGas(attacker, evm::DEFAULT_GAS_LIMIT));

    // non-owner cannot upgrade
    {
        // This revert is expected: `_authorizeUpgrade` enforces owner-only upgrade.
        const ScopedLogLevel suppress_expected_revert_logs(spdlog::level::critical);
        const auto unauthorized_upgrade = runAwaitable(
            env.io_context,
            env.evm_instance.execute(
                attacker,
                runner_proxy,
                upgrade_input,
                evm::DEFAULT_GAS_LIMIT,
                0));
        EXPECT_FALSE(unauthorized_upgrade.has_value());
    }

    // owner can upgrade
    const auto authorized_upgrade = runAwaitable(
        env.io_context,
        env.evm_instance.execute(
            env.genesis_address,
            runner_proxy,
            upgrade_input,
            evm::DEFAULT_GAS_LIMIT,
            0));
    ASSERT_TRUE(authorized_upgrade.has_value());

    // new implementation function available via proxy
    const auto version_after_upgrade = runAwaitable(
        env.io_context,
        env.evm_instance.execute(
            env.genesis_address,
            runner_proxy,
            version_input,
            evm::DEFAULT_GAS_LIMIT,
            0));

    ASSERT_TRUE(version_after_upgrade.has_value());
    EXPECT_EQ(decodeUint256Low64(version_after_upgrade.value()), 2u);

    // storage remains intact across upgrade
    const auto owner_result = runAwaitable(env.io_context, evm::fetchOwner(env.evm_instance, runner_proxy));
    ASSERT_TRUE(owner_result.has_value());

    auto owner_address_res = chain::readAddressWord(owner_result.value());
    ASSERT_TRUE(owner_address_res.has_value());

    EXPECT_EQ(owner_address_res.value(), env.genesis_address);
}

TEST_F(UnitTest, PT_ProxyUpgrade_RunnerUpgradePreservesGeneratedContext)
{
    const ScopedLogLevel scoped_log_level(spdlog::level::err);

    ProxyTestEnv env;
    registry::Registry registry(env.io_context);

    const auto runner_proxy = env.evm_instance.getRunnerAddress();
    ASSERT_FALSE(isZeroAddress(runner_proxy));

    const auto owner = makeAddressFromSuffix("pt_owner");
    runAwaitable(env.io_context, env.evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(env.io_context, env.evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto storage_path = makeProxyStoragePath(
        std::format("runner_upgrade_state_persistence_{}", std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(prepareProxyStorageDirectories(storage_path));

    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name("PersistTransformation");
    transformation_record.mutable_transformation()->set_sol_src("return x + uint32(args[0]);");
    transformation_record.set_owner(owner_hex);

    ConditionRecord condition_record;
    condition_record.mutable_condition()->set_name("PersistCondition");
    condition_record.mutable_condition()->set_sol_src("return true;");
    condition_record.set_owner(owner_hex);

    ConnectorRecord connector_record;
    connector_record.mutable_connector()->set_name("PersistConnector");
    auto * connector_dimension = connector_record.mutable_connector()->add_dimensions();
    auto * connector_transformation = connector_dimension->add_transformations();
    connector_transformation->set_name("PersistTransformation");
    connector_transformation->add_args(1);
    connector_record.mutable_connector()->set_condition_name("PersistCondition");
    connector_record.set_owner(owner_hex);

    const auto transformation_deploy_result = runAwaitable(
        env.io_context,
        loader::deployTransformation(env.evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(transformation_deploy_result.has_value())
        << std::format("deployTransformation failed: {}", transformation_deploy_result.error().kind);
    EXPECT_FALSE(isZeroAddress(transformation_deploy_result.value()));

    const auto condition_deploy_result = runAwaitable(
        env.io_context,
        loader::deployCondition(env.evm_instance, registry, condition_record, storage_path));
    ASSERT_TRUE(condition_deploy_result.has_value())
        << std::format("deployCondition failed: {}", condition_deploy_result.error().kind);
    EXPECT_FALSE(isZeroAddress(condition_deploy_result.value()));

    const auto connector_deploy_result = runAwaitable(
        env.io_context,
        loader::deployConnector(env.evm_instance, registry, connector_record, storage_path));
    ASSERT_TRUE(connector_deploy_result.has_value())
        << std::format("deployConnector failed: {}", connector_deploy_result.error().kind);
    EXPECT_FALSE(isZeroAddress(connector_deploy_result.value()));

    runAwaitable(env.io_context, env.evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto gen_input = makeRunnerGenInput("PersistConnector", 8, {{0u, 0u, 0u}});
    const auto generation_before_upgrade = runAwaitable(
        env.io_context,
        env.evm_instance.execute(
            owner,
            runner_proxy,
            gen_input,
            evm::DEFAULT_GAS_LIMIT,
            0));

    ASSERT_TRUE(generation_before_upgrade.has_value());
    const auto particles_before_upgrade_res = parse::decodeBytes<std::vector<Particles>>(generation_before_upgrade.value());
    ASSERT_TRUE(particles_before_upgrade_res.has_value());

    const auto & particles_before_upgrade = particles_before_upgrade_res.value();
    ASSERT_EQ(particles_before_upgrade.size(), 1u);
    EXPECT_EQ(particles_before_upgrade[0].path(), "/PersistConnector:0");
    ASSERT_EQ(particles_before_upgrade[0].data_size(), 8);
    for(int i = 0; i < particles_before_upgrade[0].data_size(); ++i)
    {
        EXPECT_EQ(particles_before_upgrade[0].data(i), i);
    }

    const auto runner_v2_result = deployRunnerV2(env);
    ASSERT_TRUE(runner_v2_result.has_value()) << runner_v2_result.error();

    runAwaitable(env.io_context, env.evm_instance.setGas(env.genesis_address, evm::DEFAULT_GAS_LIMIT));
    const auto upgrade_input = makeUpgradeToAndCallInput(*runner_v2_result);
    const auto authorized_upgrade = runAwaitable(
        env.io_context,
        env.evm_instance.execute(
            env.genesis_address,
            runner_proxy,
            upgrade_input,
            evm::DEFAULT_GAS_LIMIT,
            0));
    ASSERT_TRUE(authorized_upgrade.has_value());

    const auto version_after_upgrade = runAwaitable(
        env.io_context,
        env.evm_instance.execute(
            env.genesis_address,
            runner_proxy,
            chain::constructSelector("version()"),
            evm::DEFAULT_GAS_LIMIT,
            0));
    ASSERT_TRUE(version_after_upgrade.has_value());
    EXPECT_EQ(decodeUint256Low64(version_after_upgrade.value()), 2u);

    runAwaitable(env.io_context, env.evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));
    const auto generation_after_upgrade = runAwaitable(
        env.io_context,
        env.evm_instance.execute(
            owner,
            runner_proxy,
            gen_input,
            evm::DEFAULT_GAS_LIMIT,
            0));

    ASSERT_TRUE(generation_after_upgrade.has_value());
    const auto particles_after_upgrade_res = parse::decodeBytes<std::vector<Particles>>(generation_after_upgrade.value());
    ASSERT_TRUE(particles_after_upgrade_res.has_value());

    const auto & particles_after_upgrade = particles_after_upgrade_res.value();
    
    expectParticlesEqual(particles_before_upgrade, particles_after_upgrade);
}
