#include "unit-tests.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <string>
#include <tuple>
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
    constexpr const char * kIdentityTransformationName = "IdentityTx";
    constexpr const char * kIncrementTransformationName = "IncrementTx";
    const std::array<std::filesystem::path, 3> kBindingBuildDirs{
        std::filesystem::path("connectors") / "build",
        std::filesystem::path("transformations") / "build",
        std::filesystem::path("conditions") / "build",
    };

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

    chain::Address makeAddressFromSuffix(const char * suffix)
    {
        chain::Address address{};
        const std::size_t suffix_len = std::strlen(suffix);
        const std::size_t copied_len = (suffix_len <= 20) ? suffix_len : 20;
        const std::size_t suffix_offset = suffix_len - copied_len;
        std::memcpy(address.bytes + (20 - copied_len), suffix + suffix_offset, copied_len);
        return address;
    }

    chain::Address makeAddressFromByte(std::uint8_t value)
    {
        chain::Address address{};
        address.bytes[19] = value;
        return address;
    }

    std::filesystem::path makeBindingStoragePath()
    {
        return buildPath() / "tests" / "pt_binding_storage" /
            std::format("{}_{}", currentTestName(), std::chrono::steady_clock::now().time_since_epoch().count());
    }

    struct BindingStorageScope
    {
        explicit BindingStorageScope(std::filesystem::path storage_path_)
            : storage_path(std::move(storage_path_))
        {
        }

        ~BindingStorageScope()
        {
            std::error_code ec;
            std::filesystem::remove_all(storage_path, ec);
            if(ec)
            {
                spdlog::warn("Failed to cleanup binding storage '{}': {}", storage_path.string(), ec.message());
            }
        }

        std::filesystem::path storage_path;
    };

    bool prepareBindingStorageDirectories(const std::filesystem::path & storage_path)
    {
        std::error_code ec;
        std::filesystem::remove_all(storage_path, ec);
        if(ec)
        {
            spdlog::error("Failed to cleanup binding storage '{}': {}", storage_path.string(), ec.message());
            return false;
        }

        for(const auto & build_dir : kBindingBuildDirs)
        {
            ec.clear();
            std::filesystem::create_directories(storage_path / build_dir, ec);
            if(ec)
            {
                spdlog::error("Failed to create binding storage directory '{}': {}", (storage_path / build_dir).string(), ec.message());
                return false;
            }
        }

        return true;
    }

    TransformationRecord makeTransformationRecord(
        const std::string & name,
        const std::string & sol_src,
        const std::string & owner_hex)
    {
        TransformationRecord record;
        record.mutable_transformation()->set_name(name);
        record.mutable_transformation()->set_sol_src(sol_src);
        record.set_owner(owner_hex);
        return record;
    }

    TransformationRecord makeIdentityTransformationRecord(const std::string & owner_hex)
    {
        return makeTransformationRecord(kIdentityTransformationName, "return x;", owner_hex);
    }

    ConnectorRecord makeConnectorRecord(const std::string & name, const std::string & owner_hex)
    {
        ConnectorRecord record;
        record.mutable_connector()->set_name(name);
        record.set_owner(owner_hex);
        return record;
    }

    void addDimensionWithTransformation(
        ConnectorRecord & record,
        const std::string & composite,
        const std::string & transformation_name,
        std::initializer_list<std::pair<std::string, std::string>> bindings = {})
    {
        auto * dimension = record.mutable_connector()->add_dimensions();
        dimension->set_composite(composite);

        auto * transformation = dimension->add_transformations();
        transformation->set_name(transformation_name);

        for(const auto & [slot, binding_target] : bindings)
        {
            (*dimension->mutable_bindings())[slot] = binding_target;
        }
    }

    void addDimension(
        ConnectorRecord & record,
        const std::string & composite,
        std::initializer_list<std::pair<std::string, std::string>> bindings = {})
    {
        addDimensionWithTransformation(record, composite, kIdentityTransformationName, bindings);
    }

    void addCycleDimension(
        ConnectorRecord & record,
        const std::string & composite,
        std::initializer_list<std::string> cycle_tx_names)
    {
        auto * dimension = record.mutable_connector()->add_dimensions();
        dimension->set_composite(composite);
        for(const auto & tx_name : cycle_tx_names)
        {
            dimension->add_transformations()->set_name(tx_name);
        }
    }

    struct ExpectedParticle
    {
        std::string path;
        std::vector<std::uint32_t> data;
    };

    bool addIdentityTransformation(
        asio::io_context & io_context,
        registry::Registry & registry,
        const std::string & owner_hex,
        std::uint8_t address_byte)
    {
        return runAwaitable(
            io_context,
            registry.addTransformation(
                makeAddressFromByte(address_byte),
                makeIdentityTransformationRecord(owner_hex)));
    }

    bool addConnectorRecord(
        asio::io_context & io_context,
        registry::Registry & registry,
        std::uint8_t address_byte,
        ConnectorRecord record)
    {
        return runAwaitable(
            io_context,
            registry.addConnector(makeAddressFromByte(address_byte), std::move(record)));
    }

    bool addScalarConnector(
        asio::io_context & io_context,
        registry::Registry & registry,
        const std::string & name,
        const std::string & owner_hex,
        std::uint8_t address_byte,
        std::uint32_t dimensions_count = 1)
    {
        ConnectorRecord record = makeConnectorRecord(name, owner_hex);
        for(std::uint32_t i = 0; i < dimensions_count; ++i)
        {
            addDimension(record, "");
        }

        return addConnectorRecord(io_context, registry, address_byte, std::move(record));
    }

    std::expected<chain::Address, std::string> deployTransformationRecord(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        registry::Registry & registry,
        TransformationRecord record,
        const std::filesystem::path & storage_path)
    {
        auto deploy_result = runAwaitable(
            io_context,
            loader::deployTransformation(
                evm_instance,
                registry,
                std::move(record),
                storage_path));

        if(!deploy_result)
        {
            return std::unexpected(std::format("deployTransformation failed: {}", deploy_result.error().kind));
        }

        return deploy_result.value();
    }

    std::expected<chain::Address, std::string> deployIdentityTransformation(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        registry::Registry & registry,
        const std::string & owner_hex,
        const std::filesystem::path & storage_path)
    {
        return deployTransformationRecord(
            io_context,
            evm_instance,
            registry,
            makeIdentityTransformationRecord(owner_hex),
            storage_path);
    }

    std::expected<chain::Address, std::string> deployConnectorRecord(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        registry::Registry & registry,
        ConnectorRecord record,
        const std::filesystem::path & storage_path)
    {
        auto deploy_result = runAwaitable(
            io_context,
            loader::deployConnector(
                evm_instance,
                registry,
                std::move(record),
                storage_path));

        if(!deploy_result)
        {
            return std::unexpected(std::format("deployConnector failed: {}", deploy_result.error().kind));
        }

        return deploy_result.value();
    }

    std::vector<std::uint8_t> makeRunnerGenInput(
        const chain::Address & registry,
        const std::string & connector_name,
        std::uint32_t particles_count,
        const std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> & dynamic_ri)
    {
        std::vector<std::uint8_t> input_data =
            chain::constructSelector("gen(address,string,uint32,(uint32,uint32,uint32)[])");
        const std::vector<std::uint8_t> arguments = pt::abi::encodeTuple({
            pt::abi::addressComponent(registry),
            pt::abi::stringComponent(connector_name),
            pt::abi::uintComponent(particles_count),
            pt::abi::Component{.dynamic = true, .data = evm::encodeAsArg(dynamic_ri)}});
        input_data.insert(input_data.end(), arguments.begin(), arguments.end());
        return input_data;
    }

    std::expected<std::vector<Particles>, std::string> runGeneration(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        const chain::Address & owner,
        const std::string & connector_name,
        std::uint32_t particles_count)
    {
        const auto input_data = makeRunnerGenInput(evm_instance.getRegistryAddress(), connector_name, particles_count, {});
        auto execute_result = runAwaitable(
            io_context,
            evm_instance.execute(
                owner,
                evm_instance.getRunnerAddress(),
                input_data,
                evm::DEFAULT_GAS_LIMIT,
                0));

        if(!execute_result)
        {
            return std::unexpected(std::format("runner.gen failed: {}", execute_result.error().kind));
        }

        auto particles_result = parse::decodeBytes<std::vector<Particles>>(execute_result.value());
        if(!particles_result)
        {
            return std::unexpected(std::format("Failed to decode particles: {}", particles_result.error().kind));
        }

        return particles_result.value();
    }

    std::expected<pt::PTExecuteError::Kind, std::string> runGenerationExpectRevert(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        const chain::Address & owner,
        const std::string & connector_name,
        std::uint32_t particles_count,
        const std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> & dynamic_ri)
    {
        const auto input_data = makeRunnerGenInput(evm_instance.getRegistryAddress(), connector_name, particles_count, dynamic_ri);
        auto execute_result = runAwaitable(
            io_context,
            evm_instance.execute(
                owner,
                evm_instance.getRunnerAddress(),
                input_data,
                evm::DEFAULT_GAS_LIMIT,
                0));

        if(execute_result)
        {
            return std::unexpected("runner.gen unexpectedly succeeded");
        }

        if(execute_result.error().kind != chain::ExecuteError::Kind::TRANSACTION_REVERTED)
        {
            return std::unexpected(std::format(
                "runner.gen failed with non-revert status: {}",
                execute_result.error().kind));
        }

        const auto decoded_error = parse::decodeBytes<pt::PTExecuteError>(execute_result.error().result_bytes);
        if(!decoded_error)
        {
            return std::unexpected(std::format("failed to decode PT error: {}", decoded_error.error().kind));
        }

        return decoded_error->kind;
    }

    void expectIdentityData(const Particles & particles, std::uint32_t particles_count, std::uint32_t expected_value = 0)
    {
        ASSERT_EQ(particles.data_size(), static_cast<int>(particles_count));
        for(std::uint32_t i = 0; i < particles_count; ++i)
        {
            EXPECT_EQ(particles.data(static_cast<int>(i)), expected_value);
        }
    }
}

TEST_F(UnitTest, PT_Bindings_RegistryAcceptsDeepBindingToNestedSlot)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));

    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x11, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "PITCH", owner_hex, 0x12, 2));

    ConnectorRecord rhythm = makeConnectorRecord("RHYTHM", owner_hex);
    addDimension(rhythm, "TIME");
    addDimension(rhythm, "");
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x13, std::move(rhythm)));

    ConnectorRecord melody = makeConnectorRecord("MELODY", owner_hex);
    addDimension(melody, "PITCH");
    addDimension(melody, "RHYTHM");
    addDimension(melody, "");
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(melody)));

    ConnectorRecord song = makeConnectorRecord("SONG", owner_hex);
    addDimension(song, "MELODY", {{"3", "TIME"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(song)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAcceptsBindingToPropagatedStaticChildSlot)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));

    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE2", owner_hex, 0x11, 2));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TARGET2", owner_hex, 0x12, 2));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x13, 1));

    ConnectorRecord test = makeConnectorRecord("TEST", owner_hex);
    addDimension(test, "BASE2", {{"0", "TARGET2"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(test)));

    ConnectorRecord test2 = makeConnectorRecord("TEST2", owner_hex);
    addDimension(test2, "TEST", {{"2", "TIME"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(test2)));
}

TEST_F(UnitTest, PT_Bindings_RegistryComputesNestedPropagatedSlotsAcrossMixedStaticBindings)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));

    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE3", owner_hex, 0x11, 3));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "ALT", owner_hex, 0x13, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "PITCH2", owner_hex, 0x14, 2));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE3", {{"0", "TIME"}, {"2", "ALT"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(child)));

    ConnectorRecord mid = makeConnectorRecord("MID", owner_hex);
    addDimension(mid, "CHILD", {{"1", "PITCH2"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x16, std::move(mid)));

    ConnectorRecord root_valid = makeConnectorRecord("ROOT_VALID", owner_hex);
    addDimension(root_valid, "MID", {{"3", "TIME"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x17, std::move(root_valid)));

    ConnectorRecord root_invalid = makeConnectorRecord("ROOT_INVALID", owner_hex);
    addDimension(root_invalid, "MID", {{"4", "TIME"}});
    EXPECT_FALSE(addConnectorRecord(io_context, registry, 0x18, std::move(root_invalid)));
}

TEST_F(UnitTest, PT_Bindings_RegistryRejectsBindingsOnScalarDimension)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x11, 1));

    ConnectorRecord invalid = makeConnectorRecord("SCALAR_WITH_BINDING", owner_hex);
    addDimension(invalid, "", {{"0", "TIME"}});

    EXPECT_FALSE(addConnectorRecord(io_context, registry, 0x12, std::move(invalid)));
}

TEST_F(UnitTest, PT_Bindings_RegistryRejectsNonNumericSlotId)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "CHILD", owner_hex, 0x11, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));

    ConnectorRecord invalid = makeConnectorRecord("PARENT", owner_hex);
    addDimension(invalid, "CHILD", {{"dim:0", "TIME"}});

    EXPECT_FALSE(addConnectorRecord(io_context, registry, 0x13, std::move(invalid)));
}

TEST_F(UnitTest, PT_Bindings_RegistryRejectsOutOfRangeSlotId)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "CHILD", owner_hex, 0x11, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));

    ConnectorRecord invalid = makeConnectorRecord("PARENT", owner_hex);
    addDimension(invalid, "CHILD", {{"1", "TIME"}});

    EXPECT_FALSE(addConnectorRecord(io_context, registry, 0x13, std::move(invalid)));
}

TEST_F(UnitTest, PT_Bindings_RegistryRejectsMissingBindingTarget)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "CHILD", owner_hex, 0x11, 1));

    ConnectorRecord invalid = makeConnectorRecord("PARENT", owner_hex);
    addDimension(invalid, "CHILD", {{"0", "MISSING_TARGET"}});

    EXPECT_FALSE(addConnectorRecord(io_context, registry, 0x12, std::move(invalid)));
}

TEST_F(UnitTest, PT_Bindings_RegistryRejectsDuplicateCanonicalSlotIds)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "CHILD", owner_hex, 0x11, 3));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "ALT", owner_hex, 0x13, 1));

    ConnectorRecord invalid = makeConnectorRecord("PARENT", owner_hex);
    addDimension(invalid, "CHILD", {{"1", "TIME"}, {"01", "ALT"}});

    EXPECT_FALSE(addConnectorRecord(io_context, registry, 0x14, std::move(invalid)));
}

TEST_F(UnitTest, PT_Bindings_RegistryRejectsCycleThroughBindingTarget)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "LEAF", owner_hex, 0x11, 1));

    ConnectorRecord a_v1 = makeConnectorRecord("A", owner_hex);
    addDimension(a_v1, "LEAF");
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x12, std::move(a_v1)));

    ConnectorRecord a_v2 = makeConnectorRecord("A", owner_hex);
    addDimension(a_v2, "LEAF", {{"0", "A"}});

    EXPECT_FALSE(addConnectorRecord(io_context, registry, 0x13, std::move(a_v2)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsParentOverrideOfChildStaticBinding)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "LEAF", owner_hex, 0x11, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "LEAF", {{"0", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x13, std::move(child)));

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "TIME"}});

    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(parent)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsBindingRemainingUnboundChildSlots)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x11, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE", owner_hex, 0x12, 2));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE", {{"0", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x13, std::move(child)));

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "TIME"}});

    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(parent)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsBindingToConnectorWithStaticBindings)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE", owner_hex, 0x11, 2));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));

    ConnectorRecord target = makeConnectorRecord("TARGET", owner_hex);
    addDimension(target, "BASE", {{"0", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x13, std::move(target)));

    ConnectorRecord host = makeConnectorRecord("HOST", owner_hex);
    addDimension(host, "BASE");
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(host)));

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "HOST", {{"1", "TARGET"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(root)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsCompositeWithStaticBindingsReusedAcrossDimensions)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE", owner_hex, 0x11, 2));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "ALT", owner_hex, 0x13, 1));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE", {{"0", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(child)));

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "ALT"}});
    addDimension(parent, "CHILD");
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(parent)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsBindingToPropagatedSlotWhenChildHasStaticBindings)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE", owner_hex, 0x11, 2));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE", {{"0", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x13, std::move(child)));

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"1", "TIME"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(parent)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsBindingTargetWithoutOpenSlots)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE", owner_hex, 0x11, 2));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "ALT", owner_hex, 0x13, 1));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE", {{"0", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(child)));

    ConnectorRecord sealed = makeConnectorRecord("SEALED", owner_hex);
    addDimension(sealed, "CHILD", {{"0", "ALT"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(sealed)));

    ConnectorRecord host = makeConnectorRecord("HOST", owner_hex);
    addDimension(host, "BASE");
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x16, std::move(host)));

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "HOST", {{"1", "SEALED"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x17, std::move(root)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsBindingAllRemainingSlotsAfterChildStaticBindings)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE3", owner_hex, 0x11, 3));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "ALT", owner_hex, 0x13, 1));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE3", {{"1", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(child)));

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "ALT"}, {"1", "TIME"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(parent)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsBindingToMiddlePropagatedSlotAfterMultipleChildStaticBindings)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE3", owner_hex, 0x11, 3));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "ALT", owner_hex, 0x13, 1));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE3", {{"0", "TIME"}, {"2", "ALT"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(child)));

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"1", "TIME"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(parent)));
}

TEST_F(UnitTest, PT_Bindings_RegistryAllowsSameCompactedSlotIdOnDifferentDimensions)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromSuffix("bindings_owner"));

    ASSERT_TRUE(addIdentityTransformation(io_context, registry, owner_hex, 0x01));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "BASE2", owner_hex, 0x11, 2));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x12, 1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "ALT", owner_hex, 0x13, 1));

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE2", {{"0", "TIME"}});
    ASSERT_TRUE(addConnectorRecord(io_context, registry, 0x14, std::move(child)));

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "ALT"}});
    addDimension(parent, "CHILD", {{"0", "TIME"}});
    EXPECT_TRUE(addConnectorRecord(io_context, registry, 0x15, std::move(parent)));
}

TEST_F(UnitTest, PT_Bindings_CodegenSortsBindingSlotIdsNumerically)
{
    ConnectorRecord record = makeConnectorRecord("CODEGEN_SORT", "0xabc123");
    addDimension(record, "ROOT_CHILD", {
        {"10", "ZETA"},
        {"2", "BETA"},
        {"1", "ALPHA"}
    });

    auto solidity_result = constructConnectorSolidityCode(record.connector());
    ASSERT_TRUE(solidity_result.has_value());
    const std::string & solidity = *solidity_result;

    const std::size_t slot_1 = solidity.find("bindingSlotIds[0] = uint32(1);");
    const std::size_t slot_2 = solidity.find("bindingSlotIds[1] = uint32(2);");
    const std::size_t slot_10 = solidity.find("bindingSlotIds[2] = uint32(10);");

    ASSERT_NE(slot_1, std::string::npos);
    ASSERT_NE(slot_2, std::string::npos);
    ASSERT_NE(slot_10, std::string::npos);
    EXPECT_LT(slot_1, slot_2);
    EXPECT_LT(slot_2, slot_10);
}

TEST_F(UnitTest, PT_Bindings_CodegenRejectsBindingsOnScalarDimension)
{
    ConnectorRecord record = makeConnectorRecord("CODEGEN_SCALAR_INVALID", "0xabc123");
    addDimension(record, "", {{"0", "TIME"}});

    auto solidity_result = constructConnectorSolidityCode(record.connector());
    EXPECT_FALSE(solidity_result.has_value());
}

TEST_F(UnitTest, PT_Bindings_CodegenRejectsEmptyBindingTarget)
{
    ConnectorRecord record = makeConnectorRecord("CODEGEN_EMPTY_TARGET", "0xabc123");
    addDimension(record, "ROOT_CHILD", {{"0", ""}});

    auto solidity_result = constructConnectorSolidityCode(record.connector());
    EXPECT_FALSE(solidity_result.has_value());
}

TEST_F(UnitTest, PT_Bindings_RunnerResolvesDeepGrandchildBinding)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimension(pitch, "");
    addDimension(pitch, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(pitch), storage_path).has_value());

    ConnectorRecord melody = makeConnectorRecord("MELODY", owner_hex);
    addDimension(melody, "PITCH");
    addDimension(melody, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(melody), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "MELODY", {{"1", "TIME"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "ROOT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/ROOT:0/MELODY:0/PITCH:0");
    EXPECT_EQ(particles[1].path(), "/ROOT:0/MELODY:0/PITCH:1/TIME:0");
    EXPECT_EQ(particles[2].path(), "/ROOT:0/MELODY:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerResolvesBindingOnPropagatedStaticChildSlot)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord base2 = makeConnectorRecord("BASE2", owner_hex);
    addDimension(base2, "");
    addDimension(base2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base2), storage_path).has_value());

    ConnectorRecord target2 = makeConnectorRecord("TARGET2", owner_hex);
    addDimension(target2, "");
    addDimension(target2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(target2), storage_path).has_value());

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimension(pitch, "");
    addDimension(pitch, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(pitch), storage_path).has_value());

    ConnectorRecord test = makeConnectorRecord("TEST", owner_hex);
    addDimension(test, "BASE2", {{"0", "TARGET2"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(test), storage_path).has_value());

    ConnectorRecord test2 = makeConnectorRecord("TEST2", owner_hex);
    addDimension(test2, "TEST", {{"2", "PITCH"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(test2), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "TEST2", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 4u);

    EXPECT_EQ(particles[0].path(), "/TEST2:0/TEST:0/BASE2:0/TARGET2:0");
    EXPECT_EQ(particles[1].path(), "/TEST2:0/TEST:0/BASE2:0/TARGET2:1");
    EXPECT_EQ(particles[2].path(), "/TEST2:0/TEST:0/BASE2:1/PITCH:0");
    EXPECT_EQ(particles[3].path(), "/TEST2:0/TEST:0/BASE2:1/PITCH:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
    expectIdentityData(particles[3], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerMapsStaticRangeStartToInternalSlot)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord base2 = makeConnectorRecord("BASE2", owner_hex);
    addDimension(base2, "");
    addDimension(base2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base2), storage_path).has_value());

    ConnectorRecord target2 = makeConnectorRecord("TARGET2", owner_hex);
    addDimension(target2, "");
    addDimension(target2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(target2), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE2", {{"0", "TARGET2"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "ALT"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(parent), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "PARENT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/PARENT:0/CHILD:0/BASE2:0/TARGET2:0/ALT:0");
    EXPECT_EQ(particles[1].path(), "/PARENT:0/CHILD:0/BASE2:0/TARGET2:1");
    EXPECT_EQ(particles[2].path(), "/PARENT:0/CHILD:0/BASE2:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerMapsStaticRangeOffsetToInternalSlot)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord base2 = makeConnectorRecord("BASE2", owner_hex);
    addDimension(base2, "");
    addDimension(base2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base2), storage_path).has_value());

    ConnectorRecord target2 = makeConnectorRecord("TARGET2", owner_hex);
    addDimension(target2, "");
    addDimension(target2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(target2), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE2", {{"0", "TARGET2"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"1", "ALT"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(parent), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "PARENT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/PARENT:0/CHILD:0/BASE2:0/TARGET2:0");
    EXPECT_EQ(particles[1].path(), "/PARENT:0/CHILD:0/BASE2:0/TARGET2:1/ALT:0");
    EXPECT_EQ(particles[2].path(), "/PARENT:0/CHILD:0/BASE2:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerMapsMultipleBindingsIntoSameStaticCompositeOffsets)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord base2 = makeConnectorRecord("BASE2", owner_hex);
    addDimension(base2, "");
    addDimension(base2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base2), storage_path).has_value());

    ConnectorRecord target2 = makeConnectorRecord("TARGET2", owner_hex);
    addDimension(target2, "");
    addDimension(target2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(target2), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord extra = makeConnectorRecord("EXTRA", owner_hex);
    addDimension(extra, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(extra), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE2", {{"0", "TARGET2"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "ALT"}, {"1", "EXTRA"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(parent), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "PARENT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/PARENT:0/CHILD:0/BASE2:0/TARGET2:0/ALT:0");
    EXPECT_EQ(particles[1].path(), "/PARENT:0/CHILD:0/BASE2:0/TARGET2:1/EXTRA:0");
    EXPECT_EQ(particles[2].path(), "/PARENT:0/CHILD:0/BASE2:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerResolvesNestedPropagatedBindingAcrossTwoLevels)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord pitch2 = makeConnectorRecord("PITCH2", owner_hex);
    addDimension(pitch2, "");
    addDimension(pitch2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(pitch2), storage_path).has_value());

    ConnectorRecord extra2 = makeConnectorRecord("EXTRA2", owner_hex);
    addDimension(extra2, "");
    addDimension(extra2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(extra2), storage_path).has_value());

    ConnectorRecord base3 = makeConnectorRecord("BASE3", owner_hex);
    addDimension(base3, "");
    addDimension(base3, "");
    addDimension(base3, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base3), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE3", {{"0", "TIME"}, {"2", "ALT"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord mid = makeConnectorRecord("MID", owner_hex);
    addDimension(mid, "CHILD", {{"1", "PITCH2"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(mid), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "MID", {{"3", "EXTRA2"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "ROOT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 5u);

    EXPECT_EQ(particles[0].path(), "/ROOT:0/MID:0/CHILD:0/BASE3:0/TIME:0");
    EXPECT_EQ(particles[1].path(), "/ROOT:0/MID:0/CHILD:0/BASE3:1/PITCH2:0");
    EXPECT_EQ(particles[2].path(), "/ROOT:0/MID:0/CHILD:0/BASE3:1/PITCH2:1");
    EXPECT_EQ(particles[3].path(), "/ROOT:0/MID:0/CHILD:0/BASE3:2/ALT:0/EXTRA2:0");
    EXPECT_EQ(particles[4].path(), "/ROOT:0/MID:0/CHILD:0/BASE3:2/ALT:0/EXTRA2:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
    expectIdentityData(particles[3], 4);
    expectIdentityData(particles[4], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerMapsParentBindingIntoChildStaticSlotRange)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord dual = makeConnectorRecord("DUAL", owner_hex);
    addDimension(dual, "");
    addDimension(dual, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(dual), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "DUAL", {{"0", "TIME"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "ALT"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(parent), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "PARENT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 2u);

    EXPECT_EQ(particles[0].path(), "/PARENT:0/CHILD:0/DUAL:0/TIME:0/ALT:0");
    EXPECT_EQ(particles[1].path(), "/PARENT:0/CHILD:0/DUAL:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerAllowsBindingToConnectorWithStaticBindings)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord base = makeConnectorRecord("BASE", owner_hex);
    addDimension(base, "");
    addDimension(base, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base), storage_path).has_value());

    ConnectorRecord target = makeConnectorRecord("TARGET", owner_hex);
    addDimension(target, "BASE", {{"0", "TIME"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(target), storage_path).has_value());

    ConnectorRecord host = makeConnectorRecord("HOST", owner_hex);
    addDimension(host, "BASE");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(host), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "HOST", {{"1", "TARGET"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "ROOT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/ROOT:0/HOST:0/BASE:0");
    EXPECT_EQ(particles[1].path(), "/ROOT:0/HOST:0/BASE:1/TARGET:0/BASE:0/TIME:0");
    EXPECT_EQ(particles[2].path(), "/ROOT:0/HOST:0/BASE:1/TARGET:0/BASE:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerSupportsChainedBoundCompositesAsComposite)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord base = makeConnectorRecord("BASE", owner_hex);
    addDimension(base, "");
    addDimension(base, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE", {{"0", "TIME"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord mid = makeConnectorRecord("MID", owner_hex);
    addDimension(mid, "CHILD", {{"0", "ALT"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(mid), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "MID");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "ROOT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 2u);

    EXPECT_EQ(particles[0].path(), "/ROOT:0/MID:0/CHILD:0/BASE:0/TIME:0/ALT:0");
    EXPECT_EQ(particles[1].path(), "/ROOT:0/MID:0/CHILD:0/BASE:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerMapsParentBindingsAcrossChildStaticSlotRange)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord base3 = makeConnectorRecord("BASE3", owner_hex);
    addDimension(base3, "");
    addDimension(base3, "");
    addDimension(base3, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base3), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE3", {{"1", "TIME"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord parent = makeConnectorRecord("PARENT", owner_hex);
    addDimension(parent, "CHILD", {{"0", "ALT"}, {"1", "TIME"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(parent), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "PARENT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/PARENT:0/CHILD:0/BASE3:0/ALT:0");
    EXPECT_EQ(particles[1].path(), "/PARENT:0/CHILD:0/BASE3:1/TIME:0/TIME:0");
    EXPECT_EQ(particles[2].path(), "/PARENT:0/CHILD:0/BASE3:2");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerSupportsReusingBoundCompositeAcrossParentDimensionsWithDifferentBindings)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord extra = makeConnectorRecord("EXTRA", owner_hex);
    addDimension(extra, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(extra), storage_path).has_value());

    ConnectorRecord base2 = makeConnectorRecord("BASE2", owner_hex);
    addDimension(base2, "");
    addDimension(base2, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base2), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimension(child, "BASE2", {{"0", "TIME"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "CHILD", {{"0", "ALT"}});
    addDimension(root, "CHILD", {{"0", "EXTRA"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "ROOT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 4u);

    EXPECT_EQ(particles[0].path(), "/ROOT:0/CHILD:0/BASE2:0/TIME:0/ALT:0");
    EXPECT_EQ(particles[1].path(), "/ROOT:0/CHILD:0/BASE2:1");
    EXPECT_EQ(particles[2].path(), "/ROOT:1/CHILD:0/BASE2:0/TIME:0/EXTRA:0");
    EXPECT_EQ(particles[3].path(), "/ROOT:1/CHILD:0/BASE2:1");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
    expectIdentityData(particles[3], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerRejectsUnsortedAndDuplicateDynamicRunningInstances)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    const auto unsorted_result = runGenerationExpectRevert(
        io_context,
        evm_instance,
        owner,
        "TIME",
        4,
        {{2u, 0u, 0u}, {1u, 0u, 0u}});
    ASSERT_TRUE(unsorted_result.has_value()) << unsorted_result.error();
    EXPECT_EQ(*unsorted_result, pt::PTExecuteError::Kind::RUNNING_INSTANCE_NOT_SORTED);

    const auto duplicate_result = runGenerationExpectRevert(
        io_context,
        evm_instance,
        owner,
        "TIME",
        4,
        {{1u, 0u, 0u}, {1u, 2u, 0u}});
    ASSERT_TRUE(duplicate_result.has_value()) << duplicate_result.error();
    EXPECT_EQ(*duplicate_result, pt::PTExecuteError::Kind::RUNNING_INSTANCE_DUPLICATE);
}

TEST_F(UnitTest, PT_Bindings_RunnerUsesBoundCompositeRootStaticRiForScalarSlotReplacement)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto increment_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord(kIncrementTransformationName, "return x + 1;", owner_hex),
        storage_path);
    ASSERT_TRUE(increment_deploy.has_value()) << increment_deploy.error();

    ConnectorRecord bound_target = makeConnectorRecord("BOUND_TARGET", owner_hex);
    addDimensionWithTransformation(bound_target, "", kIncrementTransformationName);
    (*bound_target.mutable_connector()->mutable_static_ri())[0].set_start_point(5);
    (*bound_target.mutable_connector()->mutable_static_ri())[0].set_transformation_shift(0);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(bound_target), storage_path).has_value());

    ConnectorRecord base_slot = makeConnectorRecord("BASE_SLOT", owner_hex);
    addDimensionWithTransformation(base_slot, "", kIncrementTransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(base_slot), storage_path).has_value());

    ConnectorRecord host_bind = makeConnectorRecord("HOST_BIND", owner_hex);
    addDimensionWithTransformation(host_bind, "BASE_SLOT", kIncrementTransformationName, {{"0", "BOUND_TARGET"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(host_bind), storage_path).has_value());

    ConnectorRecord root_bind = makeConnectorRecord("ROOT_BIND", owner_hex);
    addDimensionWithTransformation(root_bind, "HOST_BIND", kIncrementTransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root_bind), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "ROOT_BIND", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 1u);

    EXPECT_EQ(particles[0].path(), "/ROOT_BIND:0/HOST_BIND:0/BASE_SLOT:0/BOUND_TARGET:0");
    ASSERT_EQ(particles[0].data_size(), 4);
    EXPECT_EQ(particles[0].data(0), 5u);
    EXPECT_EQ(particles[0].data(1), 6u);
    EXPECT_EQ(particles[0].data(2), 7u);
    EXPECT_EQ(particles[0].data(3), 8u);
}

TEST_F(UnitTest, PT_Bindings_RunnerAppliesHostStaticRiToBindingExpandedDescendantPositions)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto increment_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord(kIncrementTransformationName, "return x + 1;", owner_hex),
        storage_path);
    ASSERT_TRUE(increment_deploy.has_value()) << increment_deploy.error();

    ConnectorRecord target = makeConnectorRecord("TARGET", owner_hex);
    addDimensionWithTransformation(target, "", kIncrementTransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(target), storage_path).has_value());

    ConnectorRecord leaf = makeConnectorRecord("LEAF", owner_hex);
    addDimensionWithTransformation(leaf, "", kIncrementTransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf), storage_path).has_value());

    ConnectorRecord child = makeConnectorRecord("CHILD", owner_hex);
    addDimensionWithTransformation(child, "LEAF", kIncrementTransformationName, {{"0", "TARGET"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(child), storage_path).has_value());

    ConnectorRecord host = makeConnectorRecord("HOST", owner_hex);
    addDimensionWithTransformation(host, "CHILD", kIncrementTransformationName);
    (*host.mutable_connector()->mutable_static_ri())[4].set_start_point(7);
    (*host.mutable_connector()->mutable_static_ri())[4].set_transformation_shift(0);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(host), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "HOST", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 1u);
    EXPECT_EQ(particles[0].path(), "/HOST:0/CHILD:0/LEAF:0/TARGET:0");
    ASSERT_EQ(particles[0].data_size(), 4);
    EXPECT_EQ(particles[0].data(0), 7u);
    EXPECT_EQ(particles[0].data(1), 8u);
    EXPECT_EQ(particles[0].data(2), 9u);
    EXPECT_EQ(particles[0].data(3), 10u);
}

TEST_F(UnitTest, PT_Bindings_RunnerSupportsNestedBindingDescendantStaticRiOverride)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto add1_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord("Add1Tx", "return x + 1;", owner_hex),
        storage_path);
    ASSERT_TRUE(add1_deploy.has_value()) << add1_deploy.error();

    const auto add2_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord("Add2Tx", "return x + 2;", owner_hex),
        storage_path);
    ASSERT_TRUE(add2_deploy.has_value()) << add2_deploy.error();

    const auto add3_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord("Add3Tx", "return x + 3;", owner_hex),
        storage_path);
    ASSERT_TRUE(add3_deploy.has_value()) << add3_deploy.error();

    const auto add10_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord("Add10Tx", "return x + 10;", owner_hex),
        storage_path);
    ASSERT_TRUE(add10_deploy.has_value()) << add10_deploy.error();

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimensionWithTransformation(pitch, "", "Add1Tx");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(pitch), storage_path).has_value());

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimensionWithTransformation(time, "", "Add1Tx");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord t0 = makeConnectorRecord("T0", owner_hex);
    addDimensionWithTransformation(t0, "PITCH", "Add2Tx");
    addDimensionWithTransformation(t0, "TIME", "Add10Tx");
    (*t0.mutable_connector()->mutable_static_ri())[1].set_start_point(10);
    (*t0.mutable_connector()->mutable_static_ri())[1].set_transformation_shift(0);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(t0), storage_path).has_value());

    ConnectorRecord t1 = makeConnectorRecord("T1", owner_hex);
    addDimensionWithTransformation(t1, "PITCH", "Add3Tx");
    addDimensionWithTransformation(t1, "T0", "Add2Tx", {{"0", "TIME"}});
    // DFS position 6 in T1 points to /T1:1/T0:0/PITCH:0/TIME:0
    (*t1.mutable_connector()->mutable_static_ri())[6].set_start_point(5);
    (*t1.mutable_connector()->mutable_static_ri())[6].set_transformation_shift(0);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(t1), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "T1", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/T1:0/PITCH:0");
    ASSERT_EQ(particles[0].data_size(), 4);
    EXPECT_EQ(particles[0].data(0), 0u);
    EXPECT_EQ(particles[0].data(1), 3u);
    EXPECT_EQ(particles[0].data(2), 6u);
    EXPECT_EQ(particles[0].data(3), 9u);

    EXPECT_EQ(particles[1].path(), "/T1:1/T0:0/PITCH:0/TIME:0");
    ASSERT_EQ(particles[1].data_size(), 4);
    EXPECT_EQ(particles[1].data(0), 15u);
    EXPECT_EQ(particles[1].data(1), 19u);
    EXPECT_EQ(particles[1].data(2), 23u);
    EXPECT_EQ(particles[1].data(3), 27u);

    EXPECT_EQ(particles[2].path(), "/T1:1/T0:1/TIME:0");
    ASSERT_EQ(particles[2].data_size(), 4);
    EXPECT_EQ(particles[2].data(0), 0u);
    EXPECT_EQ(particles[2].data(1), 20u);
    EXPECT_EQ(particles[2].data(2), 40u);
    EXPECT_EQ(particles[2].data(3), 60u);
}

TEST_F(UnitTest, PT_Bindings_RunnerAllowsBindingToSealedCompositeTarget)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const auto tx_deploy = deployIdentityTransformation(io_context, evm_instance, registry, owner_hex, storage_path);
    ASSERT_TRUE(tx_deploy.has_value()) << tx_deploy.error();

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time), storage_path).has_value());

    ConnectorRecord alt = makeConnectorRecord("ALT", owner_hex);
    addDimension(alt, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(alt), storage_path).has_value());

    ConnectorRecord dual = makeConnectorRecord("DUAL", owner_hex);
    addDimension(dual, "");
    addDimension(dual, "");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(dual), storage_path).has_value());

    ConnectorRecord sealed = makeConnectorRecord("SEALED", owner_hex);
    addDimension(sealed, "DUAL", {{"0", "TIME"}, {"1", "ALT"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(sealed), storage_path).has_value());

    ConnectorRecord host = makeConnectorRecord("HOST", owner_hex);
    addDimension(host, "DUAL");
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(host), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("ROOT", owner_hex);
    addDimension(root, "HOST", {{"1", "SEALED"}});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "ROOT", 4);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/ROOT:0/HOST:0/DUAL:0");
    EXPECT_EQ(particles[1].path(), "/ROOT:0/HOST:0/DUAL:1/SEALED:0/DUAL:0/TIME:0");
    EXPECT_EQ(particles[2].path(), "/ROOT:0/HOST:0/DUAL:1/SEALED:0/DUAL:1/ALT:0");

    expectIdentityData(particles[0], 4);
    expectIdentityData(particles[1], 4);
    expectIdentityData(particles[2], 4);
}

TEST_F(UnitTest, PT_Bindings_RunnerSamplesParentFromConnectorAWithThreeDimsAcrossLargeGap)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    constexpr const char * kAdd3000TransformationName = "Add3000Tx";
    constexpr std::int32_t kSampleGap = 3000;
    // For now this is the max number of samples that can be generated
    // we want to maximize this number
    constexpr std::uint32_t kSampleCount = 28;

    const auto add3000_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord(kAdd3000TransformationName, "return x + 3000;", owner_hex),
        storage_path);
    ASSERT_TRUE(add3000_deploy.has_value()) << add3000_deploy.error();

    ConnectorRecord connector_a = makeConnectorRecord("CONNECTOR_A", owner_hex);
    addDimensionWithTransformation(connector_a, "", kAdd3000TransformationName);
    addDimensionWithTransformation(connector_a, "", kAdd3000TransformationName);
    addDimensionWithTransformation(connector_a, "", kAdd3000TransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(connector_a), storage_path).has_value());

    ConnectorRecord parent_a = makeConnectorRecord("PARENT_A", owner_hex);
    addDimensionWithTransformation(parent_a, "CONNECTOR_A", kAdd3000TransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(parent_a), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "PARENT_A", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/PARENT_A:0/CONNECTOR_A:0");
    EXPECT_EQ(particles[1].path(), "/PARENT_A:0/CONNECTOR_A:1");
    EXPECT_EQ(particles[2].path(), "/PARENT_A:0/CONNECTOR_A:2");

    for(std::size_t leaf_index = 0; leaf_index < particles.size(); ++leaf_index)
    {
        const auto & particle = particles[leaf_index];
        ASSERT_EQ(particle.data_size(), static_cast<int>(kSampleCount))
            << "leaf_index=" << leaf_index;

        for(std::uint32_t i = 0; i < kSampleCount; ++i)
        {
            EXPECT_EQ(particle.data(static_cast<int>(i)), (kSampleGap * kSampleGap) * static_cast<std::int32_t>(i))
                << "leaf_index=" << leaf_index << " i=" << i;
        }
    }
}

TEST_F(UnitTest, PT_Bindings_RunnerSamplesConnectorAStandaloneAcrossLargeGap)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    constexpr const char * kAdd3000TransformationName = "Add3000Tx";
    constexpr std::int32_t kSampleGap = 3000;
    constexpr std::uint32_t kSampleCount = 3000;

    const auto add3000_deploy = deployTransformationRecord(
        io_context,
        evm_instance,
        registry,
        makeTransformationRecord(kAdd3000TransformationName, "return x + 3000;", owner_hex),
        storage_path);
    ASSERT_TRUE(add3000_deploy.has_value()) << add3000_deploy.error();

    ConnectorRecord connector_a = makeConnectorRecord("CONNECTOR_A_STANDALONE", owner_hex);
    addDimensionWithTransformation(connector_a, "", kAdd3000TransformationName);
    addDimensionWithTransformation(connector_a, "", kAdd3000TransformationName);
    addDimensionWithTransformation(connector_a, "", kAdd3000TransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(connector_a), storage_path).has_value());

    const auto particles_result = runGeneration(io_context, evm_instance, owner, "CONNECTOR_A_STANDALONE", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 3u);

    EXPECT_EQ(particles[0].path(), "/CONNECTOR_A_STANDALONE:0");
    EXPECT_EQ(particles[1].path(), "/CONNECTOR_A_STANDALONE:1");
    EXPECT_EQ(particles[2].path(), "/CONNECTOR_A_STANDALONE:2");

    for(std::size_t leaf_index = 0; leaf_index < particles.size(); ++leaf_index)
    {
        const auto & particle = particles[leaf_index];
        ASSERT_EQ(particle.data_size(), static_cast<int>(kSampleCount))
            << "leaf_index=" << leaf_index;

        for(std::uint32_t i = 0; i < kSampleCount; ++i)
        {
            EXPECT_EQ(particle.data(static_cast<int>(i)), kSampleGap * static_cast<std::int32_t>(i))
                << "leaf_index=" << leaf_index << " i=" << i;
        }
    }
}

TEST_F(UnitTest, PT_Bindings_RunnerCyclesMultipleTransformationsPerDimension)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    constexpr const char * kAddTwoTransformationName = "AddTwoTx";
    constexpr const char * kMulThreeTransformationName = "MulThreeTx";
    constexpr const char * kSubOneTransformationName = "SubOneTx";

    ASSERT_TRUE(deployTransformationRecord(
        io_context, evm_instance, registry,
        makeTransformationRecord(kAddTwoTransformationName, "return x + 2;", owner_hex),
        storage_path).has_value());
    ASSERT_TRUE(deployTransformationRecord(
        io_context, evm_instance, registry,
        makeTransformationRecord(kMulThreeTransformationName, "return x * 3;", owner_hex),
        storage_path).has_value());
    ASSERT_TRUE(deployTransformationRecord(
        io_context, evm_instance, registry,
        makeTransformationRecord(kSubOneTransformationName, "return x - 1;", owner_hex),
        storage_path).has_value());

    ConnectorRecord cycle = makeConnectorRecord("CYCLE", owner_hex);
    {
        auto * dim = cycle.mutable_connector()->add_dimensions();
        dim->set_composite("");
        dim->add_transformations()->set_name(kAddTwoTransformationName);
        dim->add_transformations()->set_name(kMulThreeTransformationName);
        dim->add_transformations()->set_name(kSubOneTransformationName);
    }
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(cycle), storage_path).has_value());

    constexpr std::uint32_t kSampleCount = 8;
    const auto particles_result = runGeneration(io_context, evm_instance, owner, "CYCLE", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 1u);
    EXPECT_EQ(particles[0].path(), "/CYCLE:0");

    // startX = 0; per-step transformation cycles [add 2, mul 3, sub 1]:
    //   space[0] = 0           (no tx applied yet)
    //   space[1] = 0 + 2 = 2   (AddTwo, txId=0)
    //   space[2] = 2 * 3 = 6   (MulThree, txId=1)
    //   space[3] = 6 - 1 = 5   (SubOne, txId=2)
    //   space[4] = 5 + 2 = 7   (AddTwo, txId=0)
    //   space[5] = 7 * 3 = 21
    //   space[6] = 21 - 1 = 20
    //   space[7] = 20 + 2 = 22
    const std::array<std::uint32_t, kSampleCount> expected{0, 2, 6, 5, 7, 21, 20, 22};

    ASSERT_EQ(particles[0].data_size(), static_cast<int>(kSampleCount));
    for(std::uint32_t i = 0; i < kSampleCount; ++i)
    {
        EXPECT_EQ(particles[0].data(static_cast<int>(i)), expected[i]) << "i=" << i;
    }
}

TEST_F(UnitTest, PT_Bindings_RunnerSamplesChildWithUnsortedIndexesExceedingEarlierPeak)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    constexpr const char * kAddFiveTransformationName = "AddFiveTx";
    constexpr const char * kSubTwoTransformationName = "SubTwoTx";
    constexpr const char * kAddSixTransformationName = "AddSixTx";
    constexpr const char * kAddTwoTransformationName = "AddTwoTx";

    for(const auto & [name, body] : std::initializer_list<std::pair<const char *, const char *>>{
            {kAddFiveTransformationName, "return x + 5;"},
            {kSubTwoTransformationName, "return x - 2;"},
            {kAddSixTransformationName, "return x + 6;"},
            {kAddTwoTransformationName, "return x + 2;"}})
    {
        const auto deploy_result = deployTransformationRecord(
            io_context, evm_instance, registry,
            makeTransformationRecord(name, body, owner_hex),
            storage_path);
        ASSERT_TRUE(deploy_result.has_value()) << name << ": " << deploy_result.error();
    }

    ConnectorRecord leaf = makeConnectorRecord("UNSORTED_LEAF", owner_hex);
    addDimensionWithTransformation(leaf, "", kAddTwoTransformationName);
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf), storage_path).has_value());

    // Parent space cycles [+5, -2, +6]: [0, 5, 3, 9]. These become indexes
    // into UNSORTED_LEAF: unsorted after 5 -> 3, then 9 exceeds the earlier peak.
    ConnectorRecord parent = makeConnectorRecord("UNSORTED_PARENT", owner_hex);
    addCycleDimension(parent, "UNSORTED_LEAF", {kAddFiveTransformationName, kSubTwoTransformationName, kAddSixTransformationName});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(parent), storage_path).has_value());

    constexpr std::uint32_t kSampleCount = 4;
    const auto particles_result = runGeneration(io_context, evm_instance, owner, "UNSORTED_PARENT", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();

    const auto & particles = particles_result.value();
    ASSERT_EQ(particles.size(), 1u);
    EXPECT_EQ(particles[0].path(), "/UNSORTED_PARENT:0/UNSORTED_LEAF:0");

    // UNSORTED_LEAF space[k] = 2k, sampled at [0, 5, 3, 9].
    const std::array<std::uint32_t, kSampleCount> expected{0, 10, 6, 18};
    ASSERT_EQ(particles[0].data_size(), static_cast<int>(kSampleCount));
    for(std::uint32_t i = 0; i < kSampleCount; ++i)
    {
        EXPECT_EQ(particles[0].data(static_cast<int>(i)), expected[i]) << "i=" << i;
    }
}

namespace
{
    // Compare on-chain particles against hardcoded expectations.
    // Reports the leaf path and sample index on mismatch so failures
    // point at the exact element rather than a vector dump.
    void expectParticles(
        const std::vector<Particles> & actual,
        const std::vector<ExpectedParticle> & expected)
    {
        ASSERT_EQ(actual.size(), expected.size())
            << "particle count mismatch";

        for(std::size_t leaf_index = 0; leaf_index < actual.size(); ++leaf_index)
        {
            const Particles & particle = actual[leaf_index];
            const ExpectedParticle & expected_particle = expected[leaf_index];

            EXPECT_EQ(particle.path(), expected_particle.path)
                << "path mismatch at leaf_index=" << leaf_index;

            ASSERT_EQ(particle.data_size(), static_cast<int>(expected_particle.data.size()))
                << "data size mismatch at leaf_index=" << leaf_index
                << " path=" << expected_particle.path;

            for(std::size_t i = 0; i < expected_particle.data.size(); ++i)
            {
                EXPECT_EQ(particle.data(static_cast<int>(i)), expected_particle.data[i])
                    << "data[" << i << "] mismatch at leaf_index=" << leaf_index
                    << " path=" << expected_particle.path;
            }
        }
    }

    // Builds the test transformations referenced by the tree tests.
    // All transformations are deployed under shared canonical names so
    // multi-test reuse of the binding build cache is safe.
    void deployCycleTransformations(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        registry::Registry & registry,
        const std::string & owner_hex,
        const std::filesystem::path & storage_path,
        std::initializer_list<std::pair<std::string, std::string>> name_to_body)
    {
        for(const auto & [name, body] : name_to_body)
        {
            auto deploy_result = deployTransformationRecord(
                io_context, evm_instance, registry,
                makeTransformationRecord(name, body, owner_hex),
                storage_path);
            ASSERT_TRUE(deploy_result.has_value())
                << "deploy " << name << " failed: " << deploy_result.error();
        }
    }
}

TEST_F(UnitTest, PT_Bindings_RunnerTraversesHeight3ChainWithMultiTransformationCyclesPerDimension)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    deployCycleTransformations(io_context, evm_instance, registry, owner_hex, storage_path, {
        {"AddOneTx",   "return x + 1;"},
        {"AddTwoTx",   "return x + 2;"},
        {"AddThreeTx", "return x + 3;"},
        {"AddFourTx",  "return x + 4;"},
        {"AddFiveTx",  "return x + 5;"},
    });

    // Chain: ROOT (1 dim → MID) → MID (1 dim → LEAF) → LEAF (1 scalar dim).
    // Each dim cycles through a list of transformations. Cycles are
    // additive so 12-particle propagation through the tree stays under
    // uint32 (Solidity 0.8+ reverts on overflow).
    ConnectorRecord leaf = makeConnectorRecord("CHAIN_LEAF", owner_hex);
    addCycleDimension(leaf, "", {"AddOneTx", "AddTwoTx", "AddFourTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf), storage_path).has_value());

    ConnectorRecord mid = makeConnectorRecord("CHAIN_MID", owner_hex);
    addCycleDimension(mid, "CHAIN_LEAF", {"AddThreeTx", "AddFiveTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(mid), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("CHAIN_ROOT", owner_hex);
    addCycleDimension(root, "CHAIN_MID", {"AddOneTx", "AddTwoTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    // Trace (particles_count = 12, indexes = [0..11]):
    //   CHAIN_ROOT.dim0 {+1, +2} (adds 3 per pair): space[2k]=3k, space[2k+1]=3k+1
    //     space = [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16]
    //   CHAIN_MID with indexes [0,1,3,4,6,7,9,10,12,13,15,16]:
    //     CHAIN_MID.dim0 {+3, +5} (adds 8 per pair): space[2k]=8k, space[2k+1]=8k+3
    //       at root indexes = [0, 3, 11, 16, 24, 27, 35, 40, 48, 51, 59, 64]
    //   CHAIN_LEAF with indexes [0,3,11,16,24,27,35,40,48,51,59,64]:
    //     CHAIN_LEAF.dim0 {+1, +2, +4} (adds 7 per triple):
    //       space[3k]=7k, space[3k+1]=7k+1, space[3k+2]=7k+3
    //       at mid indexes = [0, 7, 24, 36, 56, 63, 80, 92, 112, 119, 136, 148]
    const std::vector<ExpectedParticle> expected{
        {"/CHAIN_ROOT:0/CHAIN_MID:0/CHAIN_LEAF:0",
         {0u, 7u, 24u, 36u, 56u, 63u, 80u, 92u, 112u, 119u, 136u, 148u}},
    };

    constexpr std::uint32_t kSampleCount = 12;
    const auto particles_result = runGeneration(io_context, evm_instance, owner, "CHAIN_ROOT", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();
    expectParticles(particles_result.value(), expected);
}

TEST_F(UnitTest, PT_Bindings_RunnerTraversesHeight3TreeWithMultipleDimensionsAtMidLevel)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    deployCycleTransformations(io_context, evm_instance, registry, owner_hex, storage_path, {
        {"AddOneTx",   "return x + 1;"},
        {"AddTwoTx",   "return x + 2;"},
        {"AddThreeTx", "return x + 3;"},
        {"AddFourTx",  "return x + 4;"},
        {"AddFiveTx",  "return x + 5;"},
    });

    // Tree:  WIDEMID_ROOT (1 dim → WIDEMID_MID)
    //          WIDEMID_MID has 2 composite dims:
    //              dim 0 → WIDEMID_LEAF_A   (scalar leaf, cycle {+1, +2})
    //              dim 1 → WIDEMID_LEAF_B   (scalar leaf, cycle {+5, +1})
    // Cycles are additive so 12-particle propagation stays under uint32.
    ConnectorRecord leaf_a = makeConnectorRecord("WIDEMID_LEAF_A", owner_hex);
    addCycleDimension(leaf_a, "", {"AddOneTx", "AddTwoTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf_a), storage_path).has_value());

    ConnectorRecord leaf_b = makeConnectorRecord("WIDEMID_LEAF_B", owner_hex);
    addCycleDimension(leaf_b, "", {"AddFiveTx", "AddOneTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf_b), storage_path).has_value());

    ConnectorRecord mid = makeConnectorRecord("WIDEMID_MID", owner_hex);
    addCycleDimension(mid, "WIDEMID_LEAF_A", {"AddTwoTx", "AddFourTx"});
    addCycleDimension(mid, "WIDEMID_LEAF_B", {"AddThreeTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(mid), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("WIDEMID_ROOT", owner_hex);
    addCycleDimension(root, "WIDEMID_MID", {"AddOneTx", "AddFiveTx", "AddThreeTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    // Trace (particles_count = 12, indexes = [0..11]):
    //   WIDEMID_ROOT.dim0 {+1, +5, +3} (adds 9 per triple):
    //     space[3k] = 9k, space[3k+1] = 9k+1, space[3k+2] = 9k+6
    //     at [0..11] = [0, 1, 6, 9, 10, 15, 18, 19, 24, 27, 28, 33]
    //   WIDEMID_MID with indexes [0,1,6,9,10,15,18,19,24,27,28,33]:
    //     WIDEMID_MID.dim0 {+2, +4} (adds 6 per pair):
    //       space[2k] = 6k, space[2k+1] = 6k+2
    //       at root indexes = [0, 2, 18, 26, 30, 44, 54, 56, 72, 80, 84, 98]
    //       WIDEMID_LEAF_A scalar {+1, +2} (adds 3 per pair):
    //         space[2k] = 3k, space[2k+1] = 3k+1
    //         at [0, 2, 18, 26, 30, 44, 54, 56, 72, 80, 84, 98]
    //            = [0, 3, 27, 39, 45, 66, 81, 84, 108, 120, 126, 147]
    //     WIDEMID_MID.dim1 {+3} (len=1 fast path): space[i] = 3i
    //       at root indexes = [0, 3, 18, 27, 30, 45, 54, 57, 72, 81, 84, 99]
    //       WIDEMID_LEAF_B scalar {+5, +1} (adds 6 per pair):
    //         space[2k] = 6k, space[2k+1] = 6k+5
    //         at [0, 3, 18, 27, 30, 45, 54, 57, 72, 81, 84, 99]
    //            = [0, 11, 54, 83, 90, 137, 162, 173, 216, 245, 252, 299]
    const std::vector<ExpectedParticle> expected{
        {"/WIDEMID_ROOT:0/WIDEMID_MID:0/WIDEMID_LEAF_A:0",
         {0u, 3u, 27u, 39u, 45u, 66u, 81u, 84u, 108u, 120u, 126u, 147u}},
        {"/WIDEMID_ROOT:0/WIDEMID_MID:1/WIDEMID_LEAF_B:0",
         {0u, 11u, 54u, 83u, 90u, 137u, 162u, 173u, 216u, 245u, 252u, 299u}},
    };

    constexpr std::uint32_t kSampleCount = 12;
    const auto particles_result = runGeneration(io_context, evm_instance, owner, "WIDEMID_ROOT", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();
    expectParticles(particles_result.value(), expected);
}

TEST_F(UnitTest, PT_Bindings_RunnerTraversesHeight3TreeBranchingAtRoot)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    deployCycleTransformations(io_context, evm_instance, registry, owner_hex, storage_path, {
        {"AddOneTx",   "return x + 1;"},
        {"AddTwoTx",   "return x + 2;"},
        {"AddThreeTx", "return x + 3;"},
        {"AddFiveTx",  "return x + 5;"},
        {"MulTwoTx",   "return x * 2;"},
    });

    // Tree:  BRANCH_ROOT has 2 composite dims:
    //          dim 0 → BRANCH_MID_A (1 dim → BRANCH_LEAF_A scalar)
    //          dim 1 → BRANCH_MID_B (2 dims → BRANCH_LEAF_B scalar, BRANCH_LEAF_C scalar)
    // Produces 3 leaves spread across two recursion subtrees.
    //
    // At 12 particles we keep BRANCH_MID_A.dim0 {+2, x2} and
    // BRANCH_LEAF_B {x2} (the latter exercises "x*2 from zero stays zero"
    // semantics), but BRANCH_ROOT.dim1 and BRANCH_MID_B.dim0 are additive
    // so the maximum index reaching LEAF_B stays small enough for the
    // runAt loop to fit under DEFAULT_GAS_LIMIT.
    ConnectorRecord leaf_a = makeConnectorRecord("BRANCH_LEAF_A", owner_hex);
    addCycleDimension(leaf_a, "", {"AddFiveTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf_a), storage_path).has_value());

    ConnectorRecord leaf_b = makeConnectorRecord("BRANCH_LEAF_B", owner_hex);
    addCycleDimension(leaf_b, "", {"MulTwoTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf_b), storage_path).has_value());

    ConnectorRecord leaf_c = makeConnectorRecord("BRANCH_LEAF_C", owner_hex);
    addCycleDimension(leaf_c, "", {"AddOneTx", "AddThreeTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(leaf_c), storage_path).has_value());

    ConnectorRecord mid_a = makeConnectorRecord("BRANCH_MID_A", owner_hex);
    addCycleDimension(mid_a, "BRANCH_LEAF_A", {"AddTwoTx", "MulTwoTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(mid_a), storage_path).has_value());

    ConnectorRecord mid_b = makeConnectorRecord("BRANCH_MID_B", owner_hex);
    addCycleDimension(mid_b, "BRANCH_LEAF_B", {"AddOneTx", "AddTwoTx", "AddOneTx"});
    addCycleDimension(mid_b, "BRANCH_LEAF_C", {"AddThreeTx", "AddOneTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(mid_b), storage_path).has_value());

    ConnectorRecord root = makeConnectorRecord("BRANCH_ROOT", owner_hex);
    addCycleDimension(root, "BRANCH_MID_A", {"AddOneTx", "AddTwoTx"});
    addCycleDimension(root, "BRANCH_MID_B", {"AddOneTx", "AddThreeTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(root), storage_path).has_value());

    // Trace (particles_count = 12, indexes = [0..11]):
    //   BRANCH_ROOT.dim0 {+1, +2} (adds 3 per pair):
    //     space[2k] = 3k, space[2k+1] = 3k+1
    //     at [0..11] = [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16]
    //   BRANCH_MID_A with those indexes:
    //     BRANCH_MID_A.dim0 {+2, x2}: f(k) = space[2k] = 4*(2^k - 1)
    //                                  space[2k+1] = 4*2^k - 2
    //       at root indexes = [0, 2, 6, 12, 28, 30, 62, 124, 252, 254, 510, 1020]
    //       BRANCH_LEAF_A scalar {+5} (len=1 fast path): space[i] = 5i
    //         at those = [0, 10, 30, 60, 140, 150, 310, 620, 1260, 1270, 2550, 5100]
    //   BRANCH_ROOT.dim1 {+1, +3} (adds 4 per pair):
    //     space[2k] = 4k, space[2k+1] = 4k+1
    //     at [0..11] = [0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21]
    //   BRANCH_MID_B with those indexes:
    //     BRANCH_MID_B.dim0 {+1, +2, +1} (adds 4 per triple):
    //       space[3k] = 4k, space[3k+1] = 4k+1, space[3k+2] = 4k+3
    //       at root indexes = [0, 1, 5, 7, 11, 12, 16, 17, 21, 23, 27, 28]
    //       BRANCH_LEAF_B scalar {x2}: x*2 of 0 stays 0 → all zeros (max iter 28)
    //     BRANCH_MID_B.dim1 {+3, +1}: space[2k] = 4k, space[2k+1] = 4k+3
    //       at root indexes = [0, 3, 8, 11, 16, 19, 24, 27, 32, 35, 40, 43]
    //       BRANCH_LEAF_C scalar {+1, +3}: space[2k] = 4k, space[2k+1] = 4k+1
    //         at those = [0, 5, 16, 21, 32, 37, 48, 53, 64, 69, 80, 85]
    const std::vector<ExpectedParticle> expected{
        {"/BRANCH_ROOT:0/BRANCH_MID_A:0/BRANCH_LEAF_A:0",
         {0u, 10u, 30u, 60u, 140u, 150u, 310u, 620u, 1260u, 1270u, 2550u, 5100u}},
        {"/BRANCH_ROOT:1/BRANCH_MID_B:0/BRANCH_LEAF_B:0",
         {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}},
        {"/BRANCH_ROOT:1/BRANCH_MID_B:1/BRANCH_LEAF_C:0",
         {0u, 5u, 16u, 21u, 32u, 37u, 48u, 53u, 64u, 69u, 80u, 85u}},
    };

    constexpr std::uint32_t kSampleCount = 12;
    const auto particles_result = runGeneration(io_context, evm_instance, owner, "BRANCH_ROOT", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();
    expectParticles(particles_result.value(), expected);
}

TEST_F(UnitTest, PT_Bindings_RunnerTraversesC1Tree_PitchAndC0WithSharedPitchAndTimeLeaves)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const BindingStorageScope storage_scope(makeBindingStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareBindingStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("binding_runtime_owner");
    const std::string owner_hex = evmc::hex(owner);

    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    deployCycleTransformations(io_context, evm_instance, registry, owner_hex, storage_path, {
        {"AddOneTx",   "return x + 1;"},
        {"AddTwoTx",   "return x + 2;"},
        {"AddThreeTx", "return x + 3;"},
        {"AddTenTx",   "return x + 10;"},
    });

    // Tree topology (user-specified structure; multiplicative cycles
    // swapped for additive so 12-particle propagation does not overflow
    // uint32 — original c0.dim0 {+1, x2} at root index >= 64 would
    // compute 2^33 and revert under Solidity 0.8+ checked arithmetic):
    //   c1
    //     ├─ dim 0 (composite = pitch, cycle: {+2})       → pitch (scalar, cycle {+1})
    //     └─ dim 1 (composite = c0,    cycle: {+1, +3})   [was {+1, x3}]
    //                                              c0
    //                                                ├─ dim 0 (composite = pitch, cycle: {+1, +2})  [was {+1, x2}]
    //                                                │                                              → pitch
    //                                                └─ dim 1 (composite = time,  cycle: {+1})       → time (scalar, cycle {+10})
    //
    // pitch is shared between c1.dim0 and c0.dim0 (same named connector).
    ConnectorRecord pitch_connector = makeConnectorRecord("pitch", owner_hex);
    addCycleDimension(pitch_connector, "", {"AddOneTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(pitch_connector), storage_path).has_value());

    ConnectorRecord time_connector = makeConnectorRecord("time", owner_hex);
    addCycleDimension(time_connector, "", {"AddTenTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(time_connector), storage_path).has_value());

    ConnectorRecord c0 = makeConnectorRecord("c0", owner_hex);
    addCycleDimension(c0, "pitch", {"AddOneTx", "AddTwoTx"});
    addCycleDimension(c0, "time",  {"AddOneTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(c0), storage_path).has_value());

    ConnectorRecord c1 = makeConnectorRecord("c1", owner_hex);
    addCycleDimension(c1, "pitch", {"AddTwoTx"});
    addCycleDimension(c1, "c0",    {"AddOneTx", "AddThreeTx"});
    ASSERT_TRUE(deployConnectorRecord(io_context, evm_instance, registry, std::move(c1), storage_path).has_value());

    // Trace (particles_count = 12, indexes = [0..11]):
    //   c1.dim0 {+2} (len=1 fast path): space[i] = 2i
    //     at [0..11] = [0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22] → pitch indexes
    //     pitch scalar {+1} (len=1): space[i] = i
    //       at those = [0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22]
    //   c1.dim1 {+1, +3} (adds 4 per pair):
    //     space[2k] = 4k, space[2k+1] = 4k+1
    //     at [0..11] = [0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21] → c0 indexes
    //     c0.dim0 {+1, +2} (adds 3 per pair):
    //       space[2k] = 3k, space[2k+1] = 3k+1
    //       at c0 indexes = [0, 1, 6, 7, 12, 13, 18, 19, 24, 25, 30, 31] → pitch indexes
    //       pitch scalar {+1}: space[i] = i
    //         at those = [0, 1, 6, 7, 12, 13, 18, 19, 24, 25, 30, 31]
    //     c0.dim1 {+1} (len=1): space[i] = i
    //       at c0 indexes = [0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21] → time indexes
    //       time scalar {+10} (len=1): space[i] = 10i
    //         at those = [0, 10, 40, 50, 80, 90, 120, 130, 160, 170, 200, 210]
    const std::vector<ExpectedParticle> expected{
        {"/c1:0/pitch:0",
         {0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u, 16u, 18u, 20u, 22u}},
        {"/c1:1/c0:0/pitch:0",
         {0u, 1u, 6u, 7u, 12u, 13u, 18u, 19u, 24u, 25u, 30u, 31u}},
        {"/c1:1/c0:1/time:0",
         {0u, 10u, 40u, 50u, 80u, 90u, 120u, 130u, 160u, 170u, 200u, 210u}},
    };

    constexpr std::uint32_t kSampleCount = 12;
    const auto particles_result = runGeneration(io_context, evm_instance, owner, "c1", kSampleCount);
    ASSERT_TRUE(particles_result.has_value()) << particles_result.error();
    expectParticles(particles_result.value(), expected);
}

