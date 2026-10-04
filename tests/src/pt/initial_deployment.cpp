#include "unit-tests.hpp"
#include "fake_solc.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <future>
#include <mutex>
#include <ranges>
#include <string_view>
#include <thread>
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

using namespace dcn;
using namespace dcn::tests;

namespace
{
    template<class AwaitableT>
    auto runAwaitable(asio::io_context & io_context, AwaitableT awaitable)
    {
        auto future = asio::co_spawn(io_context, std::move(awaitable), asio::use_future);
        io_context.restart();
        io_context.run();
        return future.get();
    }

    struct PTBootstrapSnapshot
    {
        bool success = false;
        std::string error_message;

        chain::Address registry_address{};
        chain::Address runner_address{};
        chain::Address registry_owner{};
        chain::Address runner_owner{};
        bool runner_exposes_registry = true;
        std::vector<std::uint8_t> registry_protocol_id;
        std::vector<std::uint8_t> runner_protocol_id;
        std::uint64_t registry_protocol_version = 0;
        std::uint64_t runner_protocol_version = 0;
        bool registry_rejected_zero_owner = false;
        bool runner_rejected_zero_owner = false;
        std::filesystem::path pt_out_path;
        bool pt_out_holds_compiled_pt = false;
        bool pt_out_paths_differ = false;
        bool pt_out_removed = false;
    };

    bool isZeroAddress(const chain::Address & address)
    {
        return std::ranges::all_of(address.bytes, [](std::uint8_t b) { return b == 0; });
    }

    chain::Address expectedGenesisAddress()
    {
        chain::Address genesis_address{};
        std::memcpy(genesis_address.bytes + (20 - 7), "genesis", 7);
        return genesis_address;
    }

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

    PTBootstrapSnapshot deployInitialPT()
    {
        PTBootstrapSnapshot snapshot;

        const auto solc_path = solcPath();
        const auto pt_path = ptPath();

        if(!std::filesystem::exists(solc_path))
        {
            snapshot.error_message = std::format("Missing Solidity compiler at '{}'", solc_path.string());
            return snapshot;
        }

        if(!std::filesystem::exists(pt_path / "contracts"))
        {
            snapshot.error_message = std::format("Missing PT contracts directory at '{}'", (pt_path / "contracts").string());
            return snapshot;
        }

        try
        {
            asio::io_context io_context;
            evm::EVM evm(io_context, EVMC_SHANGHAI, solc_path, pt_path);

            io_context.run();

            snapshot.registry_address = evm.getRegistryAddress();
            snapshot.runner_address = evm.getRunnerAddress();

            snapshot.pt_out_path = evm.getPTOutPath();
            snapshot.pt_out_holds_compiled_pt =
                std::filesystem::exists(snapshot.pt_out_path / "registry" / "RegistryBase.bin") &&
                std::filesystem::exists(snapshot.pt_out_path / "runner" / "Runner.bin");
            {
                // Never run, so it compiles nothing; it only claims its own output directory.
                asio::io_context idle_io_context;
                const evm::EVM idle_evm(idle_io_context, EVMC_SHANGHAI, solc_path, pt_path);
                snapshot.pt_out_paths_differ = idle_evm.getPTOutPath() != snapshot.pt_out_path;
            }

            auto registry_owner_future = asio::co_spawn(io_context, evm::fetchOwner(evm, snapshot.registry_address), asio::use_future);
            auto runner_owner_future = asio::co_spawn(io_context, evm::fetchOwner(evm, snapshot.runner_address), asio::use_future);

            io_context.restart();
            io_context.run();

            auto registry_owner_result = registry_owner_future.get();
            auto runner_owner_result = runner_owner_future.get();

            if(!registry_owner_result)
            {
                snapshot.error_message = std::format("fetchOwner(registry) failed: {}", registry_owner_result.error().kind);
                return snapshot;
            }

            if(!runner_owner_result)
            {
                snapshot.error_message = std::format("fetchOwner(runner) failed: {}", runner_owner_result.error().kind);
                return snapshot;
            }

            auto registry_owner_address_res = chain::readAddressWord(registry_owner_result.value());

            if(!registry_owner_address_res)
            {
                snapshot.error_message = std::format("readAddressWord(registry_owner_result) failed");
                return snapshot;
            }

            auto runner_owner_address_res = chain::readAddressWord(runner_owner_result.value());

            if(!runner_owner_address_res)
            {
                snapshot.error_message = std::format("readAddressWord(runner_owner_result) failed");
                return snapshot;
            }

            snapshot.registry_owner = registry_owner_address_res.value();
            snapshot.runner_owner = runner_owner_address_res.value();

            const auto call = [&](const chain::Address & address, const std::string & signature)
            {
                return runAwaitable(io_context, evm.execute(
                    expectedGenesisAddress(),
                    address,
                    chain::constructSelector(signature),
                    evm::DEFAULT_GAS_LIMIT,
                    0));
            };

            const auto registry_id = call(snapshot.registry_address, "protocolId()");
            const auto runner_id = call(snapshot.runner_address, "protocolId()");
            const auto registry_version = call(snapshot.registry_address, "protocolVersion()");
            const auto runner_version = call(snapshot.runner_address, "protocolVersion()");
            if(!registry_id || !runner_id || !registry_version || !runner_version)
            {
                snapshot.error_message = "Failed to read PT protocol identity";
                return snapshot;
            }
            if(registry_id->size() != 32 || runner_id->size() != 32 ||
                registry_version->size() != 32 || runner_version->size() != 32)
            {
                snapshot.error_message = "Invalid PT protocol identity response";
                return snapshot;
            }

            snapshot.registry_protocol_id = *registry_id;
            snapshot.runner_protocol_id = *runner_id;
            snapshot.registry_protocol_version = chain::readUint64Word(*registry_version).value_or(0);
            snapshot.runner_protocol_version = chain::readUint64Word(*runner_version).value_or(0);
            // The runner is not bound to a registry; Runner.gen is given one per call.
            snapshot.runner_exposes_registry = call(snapshot.runner_address, "registry()").has_value();

            std::vector<std::uint8_t> change_owner_input = chain::constructSelector("changeOwner(address)");
            change_owner_input.resize(change_owner_input.size() + 32, 0);
            const auto registry_zero_owner = runAwaitable(io_context, evm.execute(
                expectedGenesisAddress(),
                snapshot.registry_address,
                change_owner_input,
                evm::DEFAULT_GAS_LIMIT,
                0));
            const auto runner_zero_owner = runAwaitable(io_context, evm.execute(
                expectedGenesisAddress(),
                snapshot.runner_address,
                std::move(change_owner_input),
                evm::DEFAULT_GAS_LIMIT,
                0));
            snapshot.registry_rejected_zero_owner = !registry_zero_owner &&
                registry_zero_owner.error().kind == chain::ExecuteError::Kind::TRANSACTION_REVERTED;
            snapshot.runner_rejected_zero_owner = !runner_zero_owner &&
                runner_zero_owner.error().kind == chain::ExecuteError::Kind::TRANSACTION_REVERTED;
            snapshot.success = true;
        }
        catch(const std::exception & e)
        {
            snapshot.error_message = std::format("PT bootstrap failed: {}", e.what());
        }
        catch(...)
        {
            snapshot.error_message = "PT bootstrap failed with unknown exception";
        }

        snapshot.pt_out_removed = !snapshot.pt_out_path.empty() && !std::filesystem::exists(snapshot.pt_out_path);
        return snapshot;
    }

    const PTBootstrapSnapshot & getPTBootstrapSnapshot()
    {
        static PTBootstrapSnapshot snapshot;
        static std::once_flag once_flag;
        std::call_once(once_flag, []() { snapshot = deployInitialPT(); });
        return snapshot;
    }
}

TEST_F(UnitTest, PT_InitialDeployment_DeploysRegistryAndRunner)
{
    const auto & snapshot = getPTBootstrapSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;

    EXPECT_FALSE(isZeroAddress(snapshot.registry_address));
    EXPECT_FALSE(isZeroAddress(snapshot.runner_address));
    EXPECT_NE(snapshot.registry_address, snapshot.runner_address);
}

TEST_F(UnitTest, PT_InitialDeployment_SetsGenesisOwnerForCoreContracts)
{
    const auto & snapshot = getPTBootstrapSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;

    const auto expected_owner = expectedGenesisAddress();
    EXPECT_EQ(snapshot.registry_owner, expected_owner);
    EXPECT_EQ(snapshot.runner_owner, expected_owner);
}

TEST_F(UnitTest, PT_InitialDeployment_ExposesMatchingProtocolIdentity)
{
    const auto & snapshot = getPTBootstrapSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;
    ASSERT_EQ(snapshot.registry_protocol_id.size(), 32u);
    EXPECT_EQ(snapshot.registry_protocol_id, snapshot.runner_protocol_id);

    evmc::bytes32 expected_id{};
    constexpr std::string_view protocol_name = "hypermusic.pt";
    crypto::Keccak256::getHash(
        reinterpret_cast<const std::uint8_t*>(protocol_name.data()),
        protocol_name.size(),
        expected_id.bytes);
    EXPECT_TRUE(std::equal(
        snapshot.registry_protocol_id.begin(),
        snapshot.registry_protocol_id.end(),
        expected_id.bytes));
    EXPECT_EQ(snapshot.registry_protocol_version, 1u);
    EXPECT_EQ(snapshot.registry_protocol_version, snapshot.runner_protocol_version);
    EXPECT_FALSE(snapshot.runner_exposes_registry);
}

TEST_F(UnitTest, PT_OwnableBase_RejectsZeroOwner)
{
    const auto & snapshot = getPTBootstrapSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;
    EXPECT_TRUE(snapshot.registry_rejected_zero_owner);
    EXPECT_TRUE(snapshot.runner_rejected_zero_owner);
}

TEST_F(UnitTest, PT_InitialDeployment_CompilesIntoAPrivateDirectoryRemovedWithTheEVM)
{
    const auto & snapshot = getPTBootstrapSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;
    EXPECT_TRUE(snapshot.pt_out_holds_compiled_pt) << snapshot.pt_out_path.string();
    EXPECT_TRUE(snapshot.pt_out_paths_differ);
    EXPECT_TRUE(snapshot.pt_out_removed) << snapshot.pt_out_path.string();
}

TEST_F(UnitTest, PT_InitialDeployment_RemovesItsOutputOnlyAfterTheRunningCompilationEnds)
{
    const std::filesystem::path dir =
        std::filesystem::path(DECENTRALISED_ART_TEST_BINARY_DIR) / "tests" / "initial_deployment" / currentTestName();
    std::filesystem::remove_all(dir);
    // Still compiling when the EVM goes away, and writing into its output after that.
    const auto solc = writeFakeSolc(dir, dir / "started", std::chrono::seconds(2));

    std::filesystem::path pt_out_path;
    {
        asio::io_context io_context;
        evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solc, ptPath());
        pt_out_path = evm_instance.getPTOutPath();

        std::thread io_thread([&io_context]() { io_context.run(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while(!std::filesystem::exists(dir / "started") && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        io_context.stop();
        io_thread.join();
        ASSERT_TRUE(std::filesystem::exists(dir / "started"));
    }

    EXPECT_FALSE(std::filesystem::exists(pt_out_path)) << pt_out_path.string();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
