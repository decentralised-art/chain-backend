#include "unit-tests.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <mutex>
#include <ranges>

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

    std::filesystem::path makeDeployStoragePath()
    {
        return buildPath() / "tests" / "pt_deploy_storage" /
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    bool prepareDeployStorageDirectories(const std::filesystem::path & storage_path)
    {
        std::error_code ec;
        std::filesystem::remove_all(storage_path, ec);
        if(ec)
        {
            spdlog::error("Failed to cleanup deployment storage '{}': {}", storage_path.string(), ec.message());
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

    // Published entities carry no owner of their own: the registry is the authority,
    // so ownership is read back through getEntityOwner(kind, name).
    std::expected<chain::Address, std::string> fetchEntityOwner(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        const pt::EntityKind kind,
        const std::string & name)
    {
        std::vector<std::uint8_t> call = chain::constructSelector("getEntityOwner(uint8,string)");
        const std::vector<pt::abi::Component> args{
            pt::abi::uintComponent(static_cast<std::uint64_t>(kind)),
            pt::abi::stringComponent(name)};
        const std::vector<std::uint8_t> body = pt::abi::encodeTuple(args);
        call.insert(call.end(), body.begin(), body.end());

        const auto owner_result = runAwaitable(io_context, evm_instance.execute(
            evm_instance.getRegistryAddress(),
            evm_instance.getRegistryAddress(),
            std::move(call),
            evm::DEFAULT_GAS_LIMIT,
            0));
        if(!owner_result)
        {
            return std::unexpected(std::format("getEntityOwner failed: {}", owner_result.error().kind));
        }

        const auto owner_address_res = chain::readAddressWord(owner_result.value());
        if(!owner_address_res)
        {
            return std::unexpected("readAddressWord failed for owner result");
        }

        return owner_address_res.value();
    }

    struct PTDeployEntitiesSnapshot
    {
        bool success = false;
        std::string error_message;

        chain::Address owner{};

        chain::Address transformation_address{};
        chain::Address condition_address{};
        chain::Address connector_address{};

        chain::Address transformation_owner{};
        chain::Address condition_owner{};
        chain::Address connector_owner{};

        // EVM-log-decoded entity data (replaces the former registry-DB readbacks)
        pt::TransformationAddedEvent transformation_event{};
        pt::ConditionAddedEvent condition_event{};
        pt::ConnectorAddedEvent connector_event{};
    };

    PTDeployEntitiesSnapshot deployEntities()
    {
        PTDeployEntitiesSnapshot snapshot;

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

        const auto storage_path = makeDeployStoragePath();
        if(!prepareDeployStorageDirectories(storage_path))
        {
            snapshot.error_message = std::format("Failed to prepare deploy storage at '{}'", storage_path.string());
            return snapshot;
        }

        try
        {
            asio::io_context io_context;
            evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solc_path, pt_path);
            io_context.run();

            registry::Registry registry(io_context);

            snapshot.owner = makeAddressFromSuffix("pt_deploy_owner");
            runAwaitable(io_context, evm_instance.addAccount(snapshot.owner, evm::DEFAULT_GAS_LIMIT));
            runAwaitable(io_context, evm_instance.setGas(snapshot.owner, evm::DEFAULT_GAS_LIMIT));

            const std::string owner_hex = evmc::hex(snapshot.owner);

            TransformationRecord transformation_record;
            transformation_record.mutable_transformation()->set_name("DeployTransformation");
            transformation_record.mutable_transformation()->set_sol_src("return x + uint32(args[0]);");
            transformation_record.set_owner(owner_hex);

            ConditionRecord condition_record;
            condition_record.mutable_condition()->set_name("DeployCondition");
            condition_record.mutable_condition()->set_sol_src("return true;");
            condition_record.set_owner(owner_hex);

            ConnectorRecord connector_record;
            connector_record.mutable_connector()->set_name("DeployConnector");
            auto * connector_dimension = connector_record.mutable_connector()->add_dimensions();
            auto * connector_transformation = connector_dimension->add_transformations();
            connector_transformation->set_name("DeployTransformation");
            connector_transformation->add_args(7);
            connector_record.mutable_connector()->set_condition_name("DeployCondition");
            connector_record.set_owner(owner_hex);

            const auto transformation_deploy_result = runAwaitable(
                io_context,
                loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
            if(!transformation_deploy_result)
            {
                snapshot.error_message = std::format("deployTransformation failed: {}", transformation_deploy_result.error().kind);
                return snapshot;
            }
            snapshot.transformation_address = transformation_deploy_result.value();

            const auto condition_deploy_result = runAwaitable(
                io_context,
                loader::deployCondition(evm_instance, registry, condition_record, storage_path));
            if(!condition_deploy_result)
            {
                snapshot.error_message = std::format("deployCondition failed: {}", condition_deploy_result.error().kind);
                return snapshot;
            }
            snapshot.condition_address = condition_deploy_result.value();

            const auto connector_deploy_result = runAwaitable(
                io_context,
                loader::deployConnector(evm_instance, registry, connector_record, storage_path));
            if(!connector_deploy_result)
            {
                snapshot.error_message = std::format("deployConnector failed: {}", connector_deploy_result.error().kind);
                return snapshot;
            }
            snapshot.connector_address = connector_deploy_result.value();

            // Verify EVM presence by decoding the emitted deployment logs
            const auto logs = runAwaitable(io_context, evm_instance.getLogsSince(0, 1024));
            for(const auto & log : logs)
            {
                if(snapshot.transformation_event.name.empty())
                {
                    auto ev = pt::decodeTransformationAddedEvent(log.data_hex, log.topics);
                    if(ev && ev->name == "DeployTransformation")
                    {
                        snapshot.transformation_event = *ev;
                    }
                }
                if(snapshot.condition_event.name.empty())
                {
                    auto ev = pt::decodeConditionAddedEvent(log.data_hex, log.topics);
                    if(ev && ev->name == "DeployCondition")
                    {
                        snapshot.condition_event = *ev;
                    }
                }
                if(snapshot.connector_event.name.empty())
                {
                    auto ev = pt::decodeConnectorAddedEvent(log.data_hex, log.topics);
                    if(ev && ev->name == "DeployConnector")
                    {
                        snapshot.connector_event = *ev;
                    }
                }
            }

            if(snapshot.transformation_event.name.empty())
            {
                snapshot.error_message = "TransformationAdded event for 'DeployTransformation' not found in EVM logs";
                return snapshot;
            }
            if(snapshot.condition_event.name.empty())
            {
                snapshot.error_message = "ConditionAdded event for 'DeployCondition' not found in EVM logs";
                return snapshot;
            }
            if(snapshot.connector_event.name.empty())
            {
                snapshot.error_message = "ConnectorAdded event for 'DeployConnector' not found in EVM logs";
                return snapshot;
            }

            const auto transformation_owner_res = fetchEntityOwner(io_context, evm_instance, pt::EntityKind::TRANSFORMATION, "DeployTransformation");
            if(!transformation_owner_res)
            {
                snapshot.error_message = std::format("Transformation owner lookup failed: {}", transformation_owner_res.error());
                return snapshot;
            }
            snapshot.transformation_owner = transformation_owner_res.value();

            const auto condition_owner_res = fetchEntityOwner(io_context, evm_instance, pt::EntityKind::CONDITION, "DeployCondition");
            if(!condition_owner_res)
            {
                snapshot.error_message = std::format("Condition owner lookup failed: {}", condition_owner_res.error());
                return snapshot;
            }
            snapshot.condition_owner = condition_owner_res.value();

            const auto connector_owner_res = fetchEntityOwner(io_context, evm_instance, pt::EntityKind::CONNECTOR, "DeployConnector");
            if(!connector_owner_res)
            {
                snapshot.error_message = std::format("Connector owner lookup failed: {}", connector_owner_res.error());
                return snapshot;
            }
            snapshot.connector_owner = connector_owner_res.value();

            snapshot.success = true;
        }
        catch(const std::exception & e)
        {
            snapshot.error_message = std::format("Deploy entities flow failed: {}", e.what());
        }
        catch(...)
        {
            snapshot.error_message = "Deploy entities flow failed with unknown exception";
        }

        return snapshot;
    }

    const PTDeployEntitiesSnapshot & getPTDeployEntitiesSnapshot()
    {
        static PTDeployEntitiesSnapshot snapshot;
        static std::once_flag once_flag;
        std::call_once(once_flag, []() { snapshot = deployEntities(); });
        return snapshot;
    }
}

TEST_F(UnitTest, PT_Deploy_Transformation_DeploysAndRegisters)
{
    const auto & snapshot = getPTDeployEntitiesSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;

    EXPECT_FALSE(isZeroAddress(snapshot.transformation_address));
    EXPECT_EQ(snapshot.transformation_owner, snapshot.owner);

    // EVM-presence check: verify the TransformationAdded event was emitted on chain
    EXPECT_EQ(snapshot.transformation_event.name, "DeployTransformation");
    EXPECT_EQ(snapshot.transformation_event.args_count, 1u);
}

TEST_F(UnitTest, PT_Deploy_Condition_DeploysAndRegisters)
{
    const auto & snapshot = getPTDeployEntitiesSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;

    EXPECT_FALSE(isZeroAddress(snapshot.condition_address));
    EXPECT_EQ(snapshot.condition_owner, snapshot.owner);

    // EVM-presence check: verify the ConditionAdded event was emitted on chain
    EXPECT_EQ(snapshot.condition_event.name, "DeployCondition");
    EXPECT_EQ(snapshot.condition_event.args_count, 0u);
}

TEST_F(UnitTest, PT_Deploy_Connector_DeploysAndRegisters)
{
    const auto & snapshot = getPTDeployEntitiesSnapshot();
    ASSERT_TRUE(snapshot.success) << snapshot.error_message;

    EXPECT_FALSE(isZeroAddress(snapshot.connector_address));
    EXPECT_EQ(snapshot.connector_owner, snapshot.owner);

    // EVM-presence check: verify the ConnectorAdded event was emitted on chain
    EXPECT_EQ(snapshot.connector_event.name, "DeployConnector");
    EXPECT_EQ(snapshot.connector_event.dimensions_count, 1u);
    ASSERT_EQ(snapshot.connector_event.transformations.count(0u), 1u);
    ASSERT_EQ(snapshot.connector_event.transformations.at(0u).size(), 1u);
    EXPECT_EQ(snapshot.connector_event.transformations.at(0u)[0].name(), "DeployTransformation");
    ASSERT_EQ(snapshot.connector_event.transformations.at(0u)[0].args_size(), 1);
    EXPECT_EQ(snapshot.connector_event.transformations.at(0u)[0].args(0), 7);
    EXPECT_EQ(snapshot.connector_event.composites.count(0u), 0u); // no composite
    EXPECT_EQ(snapshot.connector_event.condition_name, "DeployCondition");
    EXPECT_EQ(snapshot.connector_event.condition_args.size(), 0u);
}

TEST_F(UnitTest, PT_Deploy_Connector_DuplicateName_ReturnsConnectorAlreadyRegistered)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts directory at '{}'", (ptPath() / "contracts").string());

    const auto storage_path = makeDeployStoragePath();
    ASSERT_TRUE(prepareDeployStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("pt_dup_owner");
    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));
    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name("DupDeployTransformation");
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(owner_hex);

    ConditionRecord condition_record;
    condition_record.mutable_condition()->set_name("DupDeployCondition");
    condition_record.mutable_condition()->set_sol_src("return true;");
    condition_record.set_owner(owner_hex);

    ConnectorRecord connector_record;
    connector_record.mutable_connector()->set_name("DupDeployConnector");
    auto * connector_dimension = connector_record.mutable_connector()->add_dimensions();
    auto * connector_transformation = connector_dimension->add_transformations();
    connector_transformation->set_name("DupDeployTransformation");
    connector_record.mutable_connector()->set_condition_name("DupDeployCondition");
    connector_record.set_owner(owner_hex);

    const auto transformation_deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(transformation_deploy_result) << std::format("deployTransformation failed: {}", transformation_deploy_result.error().kind);

    const auto condition_deploy_result = runAwaitable(
        io_context,
        loader::deployCondition(evm_instance, registry, condition_record, storage_path));
    ASSERT_TRUE(condition_deploy_result) << std::format("deployCondition failed: {}", condition_deploy_result.error().kind);

    const auto first_connector_deploy_result = runAwaitable(
        io_context,
        loader::deployConnector(evm_instance, registry, connector_record, storage_path));
    ASSERT_TRUE(first_connector_deploy_result) << std::format("first deployConnector failed: {}", first_connector_deploy_result.error().kind);

    const auto second_connector_deploy_result = runAwaitable(
        io_context,
        loader::deployConnector(evm_instance, registry, connector_record, storage_path));
    ASSERT_FALSE(second_connector_deploy_result);
    EXPECT_EQ(second_connector_deploy_result.error().kind, pt::PTDeployError::Kind::CONNECTOR_ALREADY_REGISTERED);
}

TEST_F(UnitTest, PT_Connector_ConnectorAddedEvent_ExposesStaticRunningInstances)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts directory at '{}'", (ptPath() / "contracts").string());

    const auto storage_path = makeDeployStoragePath();
    ASSERT_TRUE(prepareDeployStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("static_ri_owner");
    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));
    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name("StaticRiTransform");
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(owner_hex);

    ConnectorRecord connector_record;
    auto * connector = connector_record.mutable_connector();
    connector->set_name("StaticRiConnector");
    connector->add_dimensions()->add_transformations()->set_name("StaticRiTransform");
    (*connector->mutable_static_ri())[0].set_start_point(11);
    (*connector->mutable_static_ri())[0].set_transformation_shift(22);
    connector_record.set_owner(owner_hex);

    const auto transformation_deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(transformation_deploy_result) << std::format("deployTransformation failed: {}", transformation_deploy_result.error().kind);

    const auto connector_deploy_result = runAwaitable(
        io_context,
        loader::deployConnector(evm_instance, registry, connector_record, storage_path));
    ASSERT_TRUE(connector_deploy_result) << std::format("deployConnector failed: {}", connector_deploy_result.error().kind);

    // Reconstruct the connector's static running instances purely from the emitted ConnectorAdded log,
    // proving they are now recoverable from chain alone.
    const auto logs = runAwaitable(io_context, evm_instance.getLogsSince(0, 1024));

    std::optional<pt::ConnectorAddedEvent> decoded;
    for(const auto & log : logs)
    {
        auto candidate = pt::decodeConnectorAddedEvent(log.data_hex, log.topics);
        if(candidate && candidate->name == "StaticRiConnector")
        {
            decoded = std::move(candidate);
            break;
        }
    }

    ASSERT_TRUE(decoded.has_value()) << "ConnectorAdded event for StaticRiConnector was not decodable";
    EXPECT_EQ(decoded->name, "StaticRiConnector");
    EXPECT_EQ(decoded->dimensions_count, 1u);
    ASSERT_EQ(decoded->static_ri.size(), 1u);
    ASSERT_EQ(decoded->static_ri.count(0), 1u);
    EXPECT_EQ(decoded->static_ri.at(0).first, 11u);
    EXPECT_EQ(decoded->static_ri.at(0).second, 22u);
}

TEST_F(UnitTest, PT_Deploy_Connector_InvalidInput_CleansTemporarySoliditySourceFile)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts directory at '{}'", (ptPath() / "contracts").string());

    const auto storage_path = makeDeployStoragePath();
    ASSERT_TRUE(prepareDeployStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);

    ConnectorRecord connector_record;
    connector_record.mutable_connector()->set_name("CleanupInvalidConnector");
    connector_record.set_owner(evmc::hex(makeAddressFromSuffix("pt_cleanup_owner")));

    const auto deploy_result = runAwaitable(
        io_context,
        loader::deployConnector(evm_instance, registry, connector_record, storage_path));
    ASSERT_FALSE(deploy_result);
    EXPECT_EQ(deploy_result.error().kind, pt::PTDeployError::Kind::INVALID_INPUT);

    const auto source_path = storage_path / "connectors" / "build" / "CleanupInvalidConnector.sol";
    EXPECT_FALSE(std::filesystem::exists(source_path)) << std::format("Temporary Solidity source still exists: {}", source_path.string());
}

TEST_F(UnitTest, PT_Deploy_Transformation_InvalidInput_CleansTemporarySoliditySourceFile)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts directory at '{}'", (ptPath() / "contracts").string());

    const auto storage_path = makeDeployStoragePath();
    ASSERT_TRUE(prepareDeployStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name("bad-name");
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(evmc::hex(makeAddressFromSuffix("pt_cleanup_owner")));

    const auto deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_FALSE(deploy_result);
    EXPECT_EQ(deploy_result.error().kind, pt::PTDeployError::Kind::INVALID_INPUT);

    const auto source_path = storage_path / "transformations" / "build" / "bad-name.sol";
    EXPECT_FALSE(std::filesystem::exists(source_path)) << std::format("Temporary Solidity source still exists: {}", source_path.string());
}

TEST_F(UnitTest, PT_Deploy_TransformationAndCondition_StoreTheirRuntimeCodeInTheRegistry)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    const auto storage_path = makeDeployStoragePath();
    ASSERT_TRUE(prepareDeployStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();
    registry::Registry registry(io_context);
    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("pt_code_owner"));

    TransformationRecord transformation;
    transformation.mutable_transformation()->set_name("StoredCodeAdd");
    transformation.mutable_transformation()->set_sol_src("return x + args[0];");
    transformation.set_owner(owner_hex);

    ConditionRecord condition;
    condition.mutable_condition()->set_name("StoredCodeCheck");
    condition.mutable_condition()->set_sol_src("return args[0] > 0;");
    condition.set_owner(owner_hex);

    const auto transformation_prepared = runAwaitable(
        io_context, loader::prepareTransformationPublication(evm_instance, transformation, storage_path));
    const auto condition_prepared = runAwaitable(
        io_context, loader::prepareConditionPublication(evm_instance, condition, storage_path));
    ASSERT_TRUE(transformation_prepared);
    ASSERT_TRUE(condition_prepared);
    const std::string transformation_hash = chain::bytes32ToHex(transformation_prepared->publication.runtime_code_hash);
    const std::string condition_hash = chain::bytes32ToHex(condition_prepared->publication.runtime_code_hash);
    EXPECT_FALSE(runAwaitable(io_context, registry.getRuntimeCode(transformation_hash)).has_value());

    ASSERT_TRUE(runAwaitable(
        io_context, loader::deployTransformation(evm_instance, registry, transformation, storage_path)));
    ASSERT_TRUE(runAwaitable(
        io_context, loader::deployCondition(evm_instance, registry, condition, storage_path)));

    const auto transformation_code = runAwaitable(io_context, registry.getRuntimeCode(transformation_hash));
    const auto condition_code = runAwaitable(io_context, registry.getRuntimeCode(condition_hash));
    ASSERT_TRUE(transformation_code.has_value());
    ASSERT_TRUE(condition_code.has_value());
    EXPECT_EQ(*transformation_code, transformation_prepared->runtime_code);
    EXPECT_EQ(*condition_code, condition_prepared->runtime_code);
}
