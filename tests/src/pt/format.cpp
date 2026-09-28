#include "unit-tests.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <future>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    // NOTE: The format-hash primitives below intentionally duplicate production logic
    // from src/chain/src/format_hash.cpp as an independent reference implementation
    // for PT contract behavior. If the format-hash algorithm changes, update both
    // this test file and the chain format_hash module.
    constexpr const char * kFormatIdentityTransformation = "FmtIdentityTx";
    constexpr std::uint8_t PATH_DIM_DOMAIN = 0x10;
    constexpr std::uint8_t PATH_CONCAT_DOMAIN = 0x11;
    constexpr std::uint8_t SCALAR_PATH_LABEL_DOMAIN = 0x12;

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

    std::filesystem::path makeFormatStoragePath()
    {
        return buildPath() / "tests" / "pt_format_storage" /
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    struct FormatStorageScope
    {
        explicit FormatStorageScope(std::filesystem::path storage_path_)
            : storage_path(std::move(storage_path_))
        {
        }

        ~FormatStorageScope()
        {
            std::error_code ec;
            std::filesystem::remove_all(storage_path, ec);
        }

        std::filesystem::path storage_path;
    };

    bool prepareFormatStorageDirectories(const std::filesystem::path & storage_path)
    {
        std::error_code ec;
        std::filesystem::remove_all(storage_path, ec);
        if(ec)
        {
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
                return false;
            }
        }

        return true;
    }

    ConnectorRecord makeConnectorRecord(const std::string & name, const std::string & owner_hex)
    {
        ConnectorRecord record;
        record.mutable_connector()->set_name(name);
        record.set_owner(owner_hex);
        return record;
    }

    void addDimension(
        ConnectorRecord & record,
        const std::string & composite,
        std::initializer_list<std::pair<std::string, std::string>> bindings = {})
    {
        auto * dimension = record.mutable_connector()->add_dimensions();
        dimension->set_composite(composite);

        for(const auto & [slot, binding_target] : bindings)
        {
            (*dimension->mutable_bindings())[slot] = binding_target;
        }

        auto * transformation = dimension->add_transformations();
        transformation->set_name(kFormatIdentityTransformation);
    }

    std::vector<std::uint8_t> encodeSelectorAndStringArg(const std::string & signature, const std::string & value)
    {
        std::vector<std::uint8_t> input = chain::constructSelector(signature);

        std::vector<std::uint8_t> offset(32, 0);
        offset[31] = 0x20;
        input.insert(input.end(), offset.begin(), offset.end());

        const auto encoded_value = evm::encodeAsArg(value);
        input.insert(input.end(), encoded_value.begin(), encoded_value.end());
        return input;
    }

    std::vector<std::uint8_t> encodeSelectorAndBytes32Arg(const std::string & signature, const evmc::bytes32 & value)
    {
        std::vector<std::uint8_t> input = chain::constructSelector(signature);
        input.insert(input.end(), std::begin(value.bytes), std::end(value.bytes));
        return input;
    }

    std::vector<std::uint8_t> encodeSelectorAndBytes32UintArg(const std::string & signature, const evmc::bytes32 & hash, std::uint32_t index)
    {
        std::vector<std::uint8_t> input = chain::constructSelector(signature);
        input.insert(input.end(), std::begin(hash.bytes), std::end(hash.bytes));

        const auto encoded_index = evm::encodeAsArg(index);
        input.insert(input.end(), encoded_index.begin(), encoded_index.end());
        return input;
    }

    std::expected<std::vector<std::uint8_t>, std::string> callRegistry(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        const chain::Address & caller,
        std::vector<std::uint8_t> input_data)
    {
        auto result = runAwaitable(
            io_context,
            evm_instance.execute(
                caller,
                evm_instance.getRegistryAddress(),
                std::move(input_data),
                evm::DEFAULT_GAS_LIMIT,
                0));

        if(!result)
        {
            return std::unexpected(std::format("registry call failed: {}", result.error().kind));
        }

        return result.value();
    }

    std::expected<evmc::bytes32, std::string> callGetFormatHash(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        const chain::Address & caller,
        const std::string & connector_name)
    {
        const auto connector_input = encodeSelectorAndStringArg("getConnector(string)", connector_name);
        auto connector_output = callRegistry(io_context, evm_instance, caller, connector_input);
        if(!connector_output)
        {
            return std::unexpected(connector_output.error());
        }

        const auto connector_address = chain::readAddressWord(*connector_output);
        if(!connector_address)
        {
            return std::unexpected("failed to decode getConnector output");
        }

        const auto hash_input = chain::constructSelector("getFormatHash()");
        auto output = runAwaitable(
            io_context,
            evm_instance.execute(
                caller,
                *connector_address,
                hash_input,
                evm::DEFAULT_GAS_LIMIT,
                0));
        if(!output)
        {
            return std::unexpected(std::format("connector getFormatHash failed: {}", output.error().kind));
        }

        if(output.value().size() < sizeof(evmc::bytes32))
        {
            return std::unexpected("short output for getFormatHash");
        }

        evmc::bytes32 hash{};
        std::memcpy(hash.bytes, output.value().data(), sizeof(hash.bytes));
        return hash;
    }

    std::expected<std::size_t, std::string> callFormatConnectorsCount(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        const chain::Address & caller,
        const evmc::bytes32 & format_hash)
    {
        const auto input = encodeSelectorAndBytes32Arg("formatConnectorsCount(bytes32)", format_hash);
        auto output = callRegistry(io_context, evm_instance, caller, input);
        if(!output)
        {
            return std::unexpected(output.error());
        }

        const auto count = chain::readWordAsSizeT(output->data(), output->size(), 0);
        if(!count)
        {
            return std::unexpected("failed to decode formatConnectorsCount output");
        }

        return *count;
    }

    std::expected<chain::Address, std::string> callGetFormatConnector(
        asio::io_context & io_context,
        evm::EVM & evm_instance,
        const chain::Address & caller,
        const evmc::bytes32 & format_hash,
        std::uint32_t index)
    {
        const auto input = encodeSelectorAndBytes32UintArg("getFormatConnector(bytes32,uint256)", format_hash, index);
        auto output = callRegistry(io_context, evm_instance, caller, input);
        if(!output)
        {
            return std::unexpected(output.error());
        }

        const auto address_res = chain::readAddressWord(*output);
        if(!address_res)
        {
            return std::unexpected("failed to decode getFormatConnector output");
        }

        return *address_res;
    }

    using dcn::crypto::readUint64BE;
    using dcn::crypto::writeUint64BE;
    using dcn::crypto::writeUint32BE;

    evmc::bytes32 keccakBytes(const std::uint8_t * data, std::size_t size)
    {
        evmc::bytes32 hash{};
        crypto::Keccak256::getHash(data, size, hash.bytes);
        return hash;
    }

    evmc::bytes32 composeFormatHash(const evmc::bytes32 & lhs, const evmc::bytes32 & rhs)
    {
        const std::uint64_t lhs0 = readUint64BE(lhs.bytes + 24);
        const std::uint64_t lhs1 = readUint64BE(lhs.bytes + 16);
        const std::uint64_t lhs2 = readUint64BE(lhs.bytes + 8);
        const std::uint64_t lhs3 = readUint64BE(lhs.bytes + 0);

        const std::uint64_t rhs0 = readUint64BE(rhs.bytes + 24);
        const std::uint64_t rhs1 = readUint64BE(rhs.bytes + 16);
        const std::uint64_t rhs2 = readUint64BE(rhs.bytes + 8);
        const std::uint64_t rhs3 = readUint64BE(rhs.bytes + 0);

        evmc::bytes32 out{};
        writeUint64BE(out.bytes + 24, lhs0 + rhs0);
        writeUint64BE(out.bytes + 16, lhs1 + rhs1);
        writeUint64BE(out.bytes + 8, lhs2 + rhs2);
        writeUint64BE(out.bytes + 0, lhs3 + rhs3);
        return out;
    }

    evmc::bytes32 dimPathHash(std::uint32_t dim_id)
    {
        std::array<std::uint8_t, 5> input{};
        input[0] = PATH_DIM_DOMAIN;
        writeUint32BE(input.data() + 1, dim_id);
        return keccakBytes(input.data(), input.size());
    }

    evmc::bytes32 concatPathHash(const evmc::bytes32 & left, const evmc::bytes32 & right)
    {
        std::array<std::uint8_t, 65> input{};
        input[0] = PATH_CONCAT_DOMAIN;
        std::memcpy(input.data() + 1, left.bytes, sizeof(left.bytes));
        std::memcpy(input.data() + 33, right.bytes, sizeof(right.bytes));
        return keccakBytes(input.data(), input.size());
    }

    evmc::bytes32 scalarPathLabelHash(const evmc::bytes32 & scalar_hash, const evmc::bytes32 & path_hash)
    {
        std::array<std::uint8_t, 65> input{};
        input[0] = SCALAR_PATH_LABEL_DOMAIN;
        std::memcpy(input.data() + 1, scalar_hash.bytes, sizeof(scalar_hash.bytes));
        std::memcpy(input.data() + 33, path_hash.bytes, sizeof(path_hash.bytes));
        return keccakBytes(input.data(), input.size());
    }

    evmc::bytes32 labelHashToFormatHash(const evmc::bytes32 & label_hash)
    {
        std::array<std::uint8_t, 33> input{};
        std::memcpy(input.data() + 1, label_hash.bytes, sizeof(label_hash.bytes));

        std::uint64_t lanes[4]{};
        for(std::uint8_t domain = 0; domain < 4; ++domain)
        {
            input[0] = domain;
            const evmc::bytes32 lane_hash = keccakBytes(input.data(), input.size());
            lanes[domain] = readUint64BE(lane_hash.bytes + 24);
        }

        evmc::bytes32 out{};
        writeUint64BE(out.bytes + 24, lanes[0]);
        writeUint64BE(out.bytes + 16, lanes[1]);
        writeUint64BE(out.bytes + 8, lanes[2]);
        writeUint64BE(out.bytes + 0, lanes[3]);
        return out;
    }

    evmc::bytes32 pathHash(std::initializer_list<std::uint32_t> dim_ids)
    {
        auto it = dim_ids.begin();
        if(it == dim_ids.end())
        {
            return {};
        }

        evmc::bytes32 out = dimPathHash(*it);
        ++it;
        for(; it != dim_ids.end(); ++it)
        {
            out = concatPathHash(out, dimPathHash(*it));
        }

        return out;
    }

    evmc::bytes32 expectedFormatHash(std::initializer_list<std::pair<std::string_view, evmc::bytes32>> labels)
    {
        std::vector<evmc::bytes32> unique_label_hashes;
        unique_label_hashes.reserve(labels.size());
        for(const auto & [scalar_name, path_hash] : labels)
        {
            const evmc::bytes32 scalar_hash = keccakBytes(
                reinterpret_cast<const std::uint8_t *>(scalar_name.data()),
                scalar_name.size());
            const evmc::bytes32 label_hash = scalarPathLabelHash(scalar_hash, path_hash);
            unique_label_hashes.push_back(label_hash);
        }

        std::sort(
            unique_label_hashes.begin(),
            unique_label_hashes.end(),
            [](const evmc::bytes32 & lhs, const evmc::bytes32 & rhs)
            {
                return std::memcmp(lhs.bytes, rhs.bytes, sizeof(lhs.bytes)) < 0;
            });

        unique_label_hashes.erase(
            std::unique(
                unique_label_hashes.begin(),
                unique_label_hashes.end(),
                [](const evmc::bytes32 & lhs, const evmc::bytes32 & rhs)
                {
                    return std::memcmp(lhs.bytes, rhs.bytes, sizeof(lhs.bytes)) == 0;
                }),
            unique_label_hashes.end());

        evmc::bytes32 format_hash{};
        for(const evmc::bytes32 & label_hash : unique_label_hashes)
        {
            format_hash = composeFormatHash(format_hash, labelHashToFormatHash(label_hash));
        }

        return format_hash;
    }
    bool equalBytes32(const evmc::bytes32 & lhs, const evmc::bytes32 & rhs)
    {
        return std::memcmp(lhs.bytes, rhs.bytes, sizeof(lhs.bytes)) == 0;
    }
}

TEST_F(UnitTest, PT_Registry_FormatHash_MatchesForSeparateConnectorsWithSameProducedLabels)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const FormatStorageScope storage_scope(makeFormatStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareFormatStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("pt_format_same_labels");
    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name(kFormatIdentityTransformation);
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(owner_hex);
    const auto tx_deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(tx_deploy_result) << std::format("deployTransformation failed: {}", tx_deploy_result.error().kind);

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(runAwaitable(io_context, loader::deployConnector(evm_instance, registry, time, storage_path)).has_value());

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimension(pitch, "");
    ASSERT_TRUE(runAwaitable(io_context, loader::deployConnector(evm_instance, registry, pitch, storage_path)).has_value());

    ConnectorRecord first = makeConnectorRecord("SAME_A", owner_hex);
    addDimension(first, "TIME");
    addDimension(first, "PITCH");
    const auto first_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, first, storage_path));
    ASSERT_TRUE(first_result.has_value()) << std::format("deployConnector(SAME_A) failed: {}", first_result.error().kind);
    const chain::Address first_address = *first_result;

    ConnectorRecord second = makeConnectorRecord("SAME_B", owner_hex);
    addDimension(second, "TIME");
    addDimension(second, "PITCH");
    const auto second_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, second, storage_path));
    ASSERT_TRUE(second_result.has_value()) << std::format("deployConnector(SAME_B) failed: {}", second_result.error().kind);
    const chain::Address second_address = *second_result;

    const auto first_hash_res = callGetFormatHash(io_context, evm_instance, owner, "SAME_A");
    ASSERT_TRUE(first_hash_res.has_value()) << first_hash_res.error();
    const auto second_hash_res = callGetFormatHash(io_context, evm_instance, owner, "SAME_B");
    ASSERT_TRUE(second_hash_res.has_value()) << second_hash_res.error();

    EXPECT_TRUE(equalBytes32(*first_hash_res, *second_hash_res));

    const auto format_count_res = callFormatConnectorsCount(io_context, evm_instance, owner, *first_hash_res);
    ASSERT_TRUE(format_count_res.has_value()) << format_count_res.error();
    ASSERT_EQ(*format_count_res, 2u);

    const auto first_bucket_item = callGetFormatConnector(io_context, evm_instance, owner, *first_hash_res, 0);
    ASSERT_TRUE(first_bucket_item.has_value()) << first_bucket_item.error();
    const auto second_bucket_item = callGetFormatConnector(io_context, evm_instance, owner, *first_hash_res, 1);
    ASSERT_TRUE(second_bucket_item.has_value()) << second_bucket_item.error();

    EXPECT_TRUE(
        (*first_bucket_item == first_address && *second_bucket_item == second_address) ||
        (*first_bucket_item == second_address && *second_bucket_item == first_address));
}

TEST_F(UnitTest, PT_Registry_FormatHash_IsOrderIndependentAcrossDimensionOrder)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const FormatStorageScope storage_scope(makeFormatStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareFormatStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("pt_format_dim_order");
    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name(kFormatIdentityTransformation);
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(owner_hex);
    const auto tx_deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(tx_deploy_result) << std::format("deployTransformation failed: {}", tx_deploy_result.error().kind);

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    ASSERT_TRUE(runAwaitable(io_context, loader::deployConnector(evm_instance, registry, time, storage_path)).has_value());

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimension(pitch, "");
    ASSERT_TRUE(runAwaitable(io_context, loader::deployConnector(evm_instance, registry, pitch, storage_path)).has_value());

    ConnectorRecord left = makeConnectorRecord("LEFT", owner_hex);
    addDimension(left, "TIME");
    addDimension(left, "PITCH");
    const auto left_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, left, storage_path));
    ASSERT_TRUE(left_result.has_value()) << std::format("deployConnector(LEFT) failed: {}", left_result.error().kind);
    const chain::Address left_address = *left_result;

    ConnectorRecord right = makeConnectorRecord("RIGHT", owner_hex);
    addDimension(right, "PITCH");
    addDimension(right, "TIME");
    const auto right_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, right, storage_path));
    ASSERT_TRUE(right_result.has_value()) << std::format("deployConnector(RIGHT) failed: {}", right_result.error().kind);

    const auto left_hash_res = callGetFormatHash(io_context, evm_instance, owner, "LEFT");
    ASSERT_TRUE(left_hash_res.has_value()) << left_hash_res.error();
    const auto right_hash_res = callGetFormatHash(io_context, evm_instance, owner, "RIGHT");
    ASSERT_TRUE(right_hash_res.has_value()) << right_hash_res.error();

    EXPECT_TRUE(equalBytes32(*left_hash_res, *right_hash_res));
    EXPECT_TRUE(equalBytes32(*left_hash_res, expectedFormatHash({
        {"TIME", pathHash({0})},
        {"PITCH", pathHash({0})}
    })));
    EXPECT_TRUE(equalBytes32(*right_hash_res, expectedFormatHash({
        {"PITCH", pathHash({0})},
        {"TIME", pathHash({0})}
    })));

    const auto left_count_res = callFormatConnectorsCount(io_context, evm_instance, owner, *left_hash_res);
    ASSERT_TRUE(left_count_res.has_value()) << left_count_res.error();
    ASSERT_EQ(*left_count_res, 2u);

    const auto first_bucket_item = callGetFormatConnector(io_context, evm_instance, owner, *left_hash_res, 0);
    ASSERT_TRUE(first_bucket_item.has_value()) << first_bucket_item.error();
    const auto second_bucket_item = callGetFormatConnector(io_context, evm_instance, owner, *left_hash_res, 1);
    ASSERT_TRUE(second_bucket_item.has_value()) << second_bucket_item.error();
    EXPECT_TRUE(
        (*first_bucket_item == left_address && *second_bucket_item == *right_result) ||
        (*first_bucket_item == *right_result && *second_bucket_item == left_address));
}

TEST_F(UnitTest, PT_Registry_FormatHash_IgnoresScalarMultiplicity)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const FormatStorageScope storage_scope(makeFormatStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareFormatStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("pt_format_multiset");
    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name(kFormatIdentityTransformation);
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(owner_hex);
    const auto tx_deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(tx_deploy_result) << std::format("deployTransformation failed: {}", tx_deploy_result.error().kind);

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimension(pitch, "");
    const auto pitch_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, pitch, storage_path));
    ASSERT_TRUE(pitch_result) << std::format("deployConnector(PITCH) failed: {}", pitch_result.error().kind);

    ConnectorRecord one = makeConnectorRecord("FMT_ONE_PITCH", owner_hex);
    addDimension(one, "PITCH");
    const auto one_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, one, storage_path));
    ASSERT_TRUE(one_result) << std::format("deployConnector(FMT_ONE_PITCH) failed: {}", one_result.error().kind);

    ConnectorRecord two = makeConnectorRecord("FMT_TWO_PITCH", owner_hex);
    addDimension(two, "PITCH");
    addDimension(two, "PITCH");
    const auto two_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, two, storage_path));
    ASSERT_TRUE(two_result) << std::format("deployConnector(FMT_TWO_PITCH) failed: {}", two_result.error().kind);

    const auto one_hash_res = callGetFormatHash(io_context, evm_instance, owner, "FMT_ONE_PITCH");
    ASSERT_TRUE(one_hash_res.has_value()) << one_hash_res.error();
    const auto two_hash_res = callGetFormatHash(io_context, evm_instance, owner, "FMT_TWO_PITCH");
    ASSERT_TRUE(two_hash_res.has_value()) << two_hash_res.error();

    EXPECT_TRUE(equalBytes32(*one_hash_res, *two_hash_res));
    EXPECT_TRUE(equalBytes32(*one_hash_res, expectedFormatHash({
        {"PITCH", pathHash({0})}
    })));
    EXPECT_TRUE(equalBytes32(*two_hash_res, expectedFormatHash({
        {"PITCH", pathHash({0})},
        {"PITCH", pathHash({0})}
    })));
}

TEST_F(UnitTest, PT_Registry_FormatHash_IsPathSensitiveAcrossBindingSlots)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const FormatStorageScope storage_scope(makeFormatStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareFormatStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("pt_format_slot_bind");
    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name(kFormatIdentityTransformation);
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(owner_hex);
    const auto tx_deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(tx_deploy_result) << std::format("deployTransformation failed: {}", tx_deploy_result.error().kind);

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimension(pitch, "");
    ASSERT_TRUE(runAwaitable(io_context, loader::deployConnector(evm_instance, registry, pitch, storage_path)).has_value());

    ConnectorRecord base2 = makeConnectorRecord("BASE2", owner_hex);
    addDimension(base2, "");
    addDimension(base2, "");
    ASSERT_TRUE(runAwaitable(io_context, loader::deployConnector(evm_instance, registry, base2, storage_path)).has_value());

    ConnectorRecord bind_slot0 = makeConnectorRecord("BIND_SLOT0", owner_hex);
    addDimension(bind_slot0, "BASE2", {{"0", "PITCH"}});
    const auto bind_slot0_res = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, bind_slot0, storage_path));
    ASSERT_TRUE(bind_slot0_res.has_value()) << std::format("deployConnector(BIND_SLOT0) failed: {}", bind_slot0_res.error().kind);

    ConnectorRecord bind_slot1 = makeConnectorRecord("BIND_SLOT1", owner_hex);
    addDimension(bind_slot1, "BASE2", {{"1", "PITCH"}});
    const auto bind_slot1_res = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, bind_slot1, storage_path));
    ASSERT_TRUE(bind_slot1_res.has_value()) << std::format("deployConnector(BIND_SLOT1) failed: {}", bind_slot1_res.error().kind);

    const auto slot0_hash_res = callGetFormatHash(io_context, evm_instance, owner, "BIND_SLOT0");
    ASSERT_TRUE(slot0_hash_res.has_value()) << slot0_hash_res.error();
    const auto slot1_hash_res = callGetFormatHash(io_context, evm_instance, owner, "BIND_SLOT1");
    ASSERT_TRUE(slot1_hash_res.has_value()) << slot1_hash_res.error();

    EXPECT_FALSE(equalBytes32(*slot0_hash_res, *slot1_hash_res));
    EXPECT_TRUE(equalBytes32(*slot0_hash_res, expectedFormatHash({
        {"PITCH", pathHash({0})},
        {"BASE2", pathHash({1})}
    })));
    EXPECT_TRUE(equalBytes32(*slot1_hash_res, expectedFormatHash({
        {"BASE2", pathHash({0})},
        {"PITCH", pathHash({0})}
    })));
}

TEST_F(UnitTest, PT_Registry_FormatHash_MatchesForSameScalarNamesWhenTailLabelsMatch)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const FormatStorageScope storage_scope(makeFormatStoragePath());
    const std::filesystem::path & storage_path = storage_scope.storage_path;
    ASSERT_TRUE(prepareFormatStorageDirectories(storage_path));

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);
    const chain::Address owner = makeAddressFromSuffix("pt_format_paths");
    runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    const std::string owner_hex = evmc::hex(owner);

    TransformationRecord transformation_record;
    transformation_record.mutable_transformation()->set_name(kFormatIdentityTransformation);
    transformation_record.mutable_transformation()->set_sol_src("return x;");
    transformation_record.set_owner(owner_hex);
    const auto tx_deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation_record, storage_path));
    ASSERT_TRUE(tx_deploy_result) << std::format("deployTransformation failed: {}", tx_deploy_result.error().kind);

    ConnectorRecord time = makeConnectorRecord("TIME", owner_hex);
    addDimension(time, "");
    const auto time_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, time, storage_path));
    ASSERT_TRUE(time_result) << std::format("deployConnector(TIME) failed: {}", time_result.error().kind);

    ConnectorRecord pitch = makeConnectorRecord("PITCH", owner_hex);
    addDimension(pitch, "");
    const auto pitch_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, pitch, storage_path));
    ASSERT_TRUE(pitch_result) << std::format("deployConnector(PITCH) failed: {}", pitch_result.error().kind);

    ConnectorRecord base2 = makeConnectorRecord("BASE2", owner_hex);
    addDimension(base2, "");
    addDimension(base2, "");
    const auto base2_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, base2, storage_path));
    ASSERT_TRUE(base2_result) << std::format("deployConnector(BASE2) failed: {}", base2_result.error().kind);

    ConnectorRecord direct = makeConnectorRecord("DIRECT", owner_hex);
    addDimension(direct, "TIME");
    addDimension(direct, "PITCH");
    const auto direct_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, direct, storage_path));
    ASSERT_TRUE(direct_result) << std::format("deployConnector(DIRECT) failed: {}", direct_result.error().kind);

    ConnectorRecord bound = makeConnectorRecord("BOUND", owner_hex);
    addDimension(bound, "BASE2", {{"0", "TIME"}, {"1", "PITCH"}});
    const auto bound_result = runAwaitable(io_context, loader::deployConnector(evm_instance, registry, bound, storage_path));
    ASSERT_TRUE(bound_result) << std::format("deployConnector(BOUND) failed: {}", bound_result.error().kind);

    const auto direct_hash_res = callGetFormatHash(io_context, evm_instance, owner, "DIRECT");
    ASSERT_TRUE(direct_hash_res.has_value()) << direct_hash_res.error();
    const auto bound_hash_res = callGetFormatHash(io_context, evm_instance, owner, "BOUND");
    ASSERT_TRUE(bound_hash_res.has_value()) << bound_hash_res.error();

    EXPECT_TRUE(equalBytes32(*direct_hash_res, *bound_hash_res));

    EXPECT_TRUE(equalBytes32(*direct_hash_res, expectedFormatHash({
        {"TIME", pathHash({0})},
        {"PITCH", pathHash({0})}
    })));
    EXPECT_TRUE(equalBytes32(*bound_hash_res, expectedFormatHash({
        {"TIME", pathHash({0})},
        {"PITCH", pathHash({0})}
    })));
}
