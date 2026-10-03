#include "unit-tests.hpp"
#include "events_test_harness.hpp"
#include "events_sql_assertions.hpp"
#include "fake_eth_node.hpp"

#include "registry_projector.hpp"
#include "registry_events.hpp"

#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

// Forward declarations from tests/src/pt/connector.cpp (same test binary, no ODR violation
// since the struct definition is identical).
namespace dcn::tests
{
    struct EncodedConnectorAddedEvent
    {
        std::string data_hex;
        std::vector<std::string> topics_hex;
    };

    EncodedConnectorAddedEvent encodeConnectorAddedForTest(const ConnectorRecord & record);
}

namespace
{
    template<class AwaitableT>
    auto runAw(asio::io_context & io, AwaitableT aw)
    {
        auto future = asio::co_spawn(io, std::move(aw), asio::use_future);
        io.restart();
        io.run();
        return future.get();
    }

    // A transformation publication of `name` by 0x42, its publish call, and the address the
    // test registry deploys it to.
    struct ChainPublication
    {
        pt::Publication publication;
        std::vector<std::uint8_t> creation_code;
        chain::Address entity{};
        std::vector<std::uint8_t> call;
    };

    ChainPublication makeChainPublication(
        const std::string & name,
        const std::uint8_t code_byte,
        const evmc::bytes32 & content_hash,
        const evmc::bytes32 & runtime_code_hash)
    {
        ChainPublication out;
        out.creation_code = {0x60, 0x80, 0x60, 0x40, 0x52, code_byte};
        out.publication.kind = pt::EntityKind::TRANSFORMATION;
        out.publication.name = name;
        out.publication.owner = makeAddressFromByte(0x42);
        out.publication.content_hash = content_hash;
        out.publication.metadata_hash = fake_eth::bytes32OfFill(0xC1);
        out.publication.creation_code_hash = chain::keccakBytes(out.creation_code);
        out.publication.runtime_code_hash = runtime_code_hash;
        out.entity = pt::predictEntityAddress(
            evmc::from_hex<chain::Address>(fake_eth::REGISTRY_ADDRESS).value(), out.publication, out.creation_code);
        out.call = pt::encodePublishTransformationCall(out.creation_code, out.publication, {});
        return out;
    }
}

// ---------------------------------------------------------------------------
// Test 1: TransformationAdded event is materialised into the registry via the
//         changelog cursor after finalization.
// ---------------------------------------------------------------------------
TEST(RegistryProjection, Transformation_IsMaterializedAndJobDeleted)
{
    const auto paths = makeTempEventsPaths("reg_proj_tf");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    // Build real ABI payload for TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)
    const chain::Address caller = makeAddressFromByte(0x41);
    const chain::Address tf_addr = makeAddressFromByte(0x43);
    const chain::Address owner  = makeAddressFromByte(0x42);
    const std::string    name   = "scale_tf_proj";
    const std::uint32_t  argc   = 0;

    const std::string data_hex = encodeSimpleAddedEventDataV2(caller, name, tf_addr, owner, argc);

    // makeDecodedEvent already sets topics[0] to the TransformationAdded sig hash.
    events::DecodedEvent event = makeDecodedEvent(
        100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    event.raw.data_hex = data_hex;          // override with real ABI payload
    event.name         = name;
    event.owner        = chain::addressToHex(owner);

    const events::ChainBlockInfo block =
        makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {event}, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 100, 100, 1'700'000'502'000));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand);

    const std::size_t projected = runAw(io, projector.projectBatch(10, 1'700'000'503'000));
    EXPECT_EQ(projected, 1u);

    EXPECT_TRUE(runAw(io, registry.hasTransformation(name)));
    EXPECT_GT(projector.cursor(), 0);
}

// ---------------------------------------------------------------------------
// Test 2: ConnectorAdded is materialised after its transformation dependency.
//         The format hash is verified via the registry's own stored value.
//
// ponytail: composite-aware format hash coverage deferred; scalar+transform
//           sufficient for full end-to-end round-trip via the registry path.
// ---------------------------------------------------------------------------
TEST(RegistryProjection, Connector_IsMaterializedWithTransformationDepAndFormatHashPresent)
{
    const auto paths = makeTempEventsPaths("reg_proj_conn");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    // ---- TransformationAdded at block 100 ----
    const chain::Address tf_owner = makeAddressFromByte(0x42);
    const chain::Address tf_addr  = makeAddressFromByte(0x43);
    const std::string    tf_name  = "scale_tf_conn_proj";

    const std::string tf_data =
        encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), tf_name, tf_addr, tf_owner, 2);

    events::DecodedEvent tf_event =
        makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    tf_event.raw.data_hex = tf_data;
    tf_event.name         = tf_name;
    tf_event.owner        = chain::addressToHex(tf_owner);

    const events::ChainBlockInfo tf_block =
        makeBlockInfo(100, tf_event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {tf_event}, {tf_block}, 101, 1'700'000'501'100));

    // ---- ConnectorAdded at block 101 ----
    // Scalar connector with one dimension that references tf_name as a transformation.
    ConnectorRecord conn_record;
    {
        chain::Address conn_owner{};
        conn_owner.bytes[19] = 0xAB;
        conn_record.set_owner(chain::addressToHex(conn_owner));
        Connector * c = conn_record.mutable_connector();
        c->set_name("test_connector_proj");
        Dimension * dim = c->add_dimensions();
        dim->set_composite(""); // scalar dimension
        TransformationDef * tdef = dim->add_transformations();
        tdef->set_name(tf_name);
        tdef->add_args(2);
        tdef->add_args(3);
    }

    const auto enc = dcn::tests::encodeConnectorAddedForTest(conn_record);

    events::DecodedEvent conn_event =
        makeDecodedEvent(101, 0, 1, 0xCC, 0xDD, events::EventType::CONNECTOR_ADDED);
    conn_event.raw.data_hex   = enc.data_hex;
    conn_event.raw.topics[0]  = enc.topics_hex[0]; // ConnectorAdded event-sig hash
    conn_event.raw.topics[1]  = enc.topics_hex[1]; // caller (indexed)
    conn_event.raw.topics[2]  = enc.topics_hex[2]; // owner  (indexed)
    conn_event.name           = "test_connector_proj";

    const events::ChainBlockInfo conn_block =
        makeBlockInfo(101, conn_event.raw.block_hash, hexBytes(0x92, 32), 1'700'000'510, 1'700'000'511'000);

    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {conn_event}, {conn_block}, 102, 1'700'000'511'100));

    // Finalize both blocks.
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 101, 101, 1'700'000'512'000));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand);

    const std::size_t projected = runAw(io, projector.projectBatch(10, 1'700'000'513'000));
    EXPECT_EQ(projected, 2u);

    EXPECT_TRUE(runAw(io, registry.hasTransformation(tf_name)));
    EXPECT_TRUE(runAw(io, registry.hasConnector("test_connector_proj")));

    // Assert format hash is present via registry's own stored value.
    const auto hash_opt = runAw(io, registry.getFormatHash("test_connector_proj"));
    ASSERT_TRUE(hash_opt.has_value());
    bool is_nonzero = false;
    for(const auto b : hash_opt->bytes)
    {
        if(b != 0)
        {
            is_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(is_nonzero);
    EXPECT_GT(projector.cursor(), 0);
}

// ---------------------------------------------------------------------------
// Test 3: RegistryProjector reads from the changelog cursor.
//         - Non-finalized events are skipped; cursor advances past them.
//         - After finalization (row re-surfaces with higher change_seq),
//           the next projectBatch materializes the event.
// ---------------------------------------------------------------------------
TEST_F(UnitTest, Events_RegistryProjector_ChangelogCursor_MaterializesOnFinalize)
{
    const auto paths = makeTempEventsPaths("reg_proj_changelog_cursor");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    const chain::Address caller  = makeAddressFromByte(0x41);
    const chain::Address tf_addr = makeAddressFromByte(0x43);
    const chain::Address owner   = makeAddressFromByte(0x42);
    const std::string    name    = "changelog_cursor_tf";
    const std::uint32_t  argc    = 0;

    const std::string data_hex = encodeSimpleAddedEventDataV2(caller, name, tf_addr, owner, argc);

    events::DecodedEvent event = makeDecodedEvent(
        100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    event.raw.data_hex = data_hex;
    event.name         = name;
    event.owner        = chain::addressToHex(owner);

    const events::ChainBlockInfo block =
        makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    // --- Step 1: Ingest as OBSERVED (not yet finalized) ---
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {event}, {block}, 101, 1'700'000'501'100));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand);

    EXPECT_EQ(projector.cursor(), 0);

    // --- Step 2: projectBatch skips the non-finalized row but advances cursor past it ---
    const std::size_t projected_first = runAw(io, projector.projectBatch(10, 1'700'000'502'000));
    EXPECT_GE(projected_first, 1u);                          // row was consumed (skipped)
    EXPECT_FALSE(runAw(io, registry.hasTransformation(name))); // NOT materialized yet
    EXPECT_GT(projector.cursor(), 0);                         // cursor advanced past observed row

    // --- Step 3: applyFinality re-surfaces the row with a higher change_seq ---
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 100, 100, 1'700'000'503'000));

    const std::int64_t cursor_after_skip = projector.cursor();

    // --- Step 4: projectBatch now finds the finalized row and materializes it ---
    const std::size_t projected_second = runAw(io, projector.projectBatch(10, 1'700'000'504'000));
    EXPECT_GE(projected_second, 1u);
    EXPECT_TRUE(runAw(io, registry.hasTransformation(name)));
    EXPECT_GT(projector.cursor(), cursor_after_skip); // cursor advanced beyond finalized row
}

// ---------------------------------------------------------------------------
// A chain connector whose transformation and condition were registered before the
// ingested block range gets them read from the registry state at the connector's block
// and stored as chain versions, so the connector materializes with its dependencies.
// ---------------------------------------------------------------------------
TEST(RegistryProjection, ChainConnector_BackfillsMissingDependenciesFromRegistryState)
{
    using namespace dcn::tests::fake_eth;

    const auto paths = makeTempEventsPaths("reg_proj_backfill");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);
    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);

    auto node = std::make_shared<FakeNode>();
    node->call_override = [](const json & request) -> std::string
    {
        const std::string data = request.at("params").at(0).value("data", "");
        if(data.starts_with(selectorOf("getEntityMetadataHash(uint8,string)"))) return rpcResult(request, wordOfFill(0xBB));
        if(data.starts_with(selectorOf("getArgsCount()"))) return rpcResult(request, wordOfUint(2));
        return {};
    };

    ConnectorRecord conn_record;
    conn_record.set_owner(hexAddress(0x42));
    conn_record.mutable_connector()->set_name("backfill_connector");
    conn_record.mutable_connector()->set_condition_name("early_cond");
    conn_record.mutable_connector()->add_dimensions()->add_transformations()->set_name("early_tf");
    const auto enc = dcn::tests::encodeConnectorAddedForTest(conn_record);

    events::DecodedEvent conn_event =
        makeDecodedEvent(101, 0, 1, 0xCC, 0xDD, events::EventType::CONNECTOR_ADDED);
    conn_event.raw.source_id = source_id;
    conn_event.raw.address = REGISTRY_ADDRESS;
    conn_event.raw.data_hex = enc.data_hex;
    conn_event.raw.topics[0] = enc.topics_hex[0];
    conn_event.raw.topics[1] = enc.topics_hex[1];
    conn_event.raw.topics[2] = enc.topics_hex[2];
    conn_event.name = "backfill_connector";

    const events::ChainBlockInfo conn_block =
        makeBlockInfo(101, conn_event.raw.block_hash, hexBytes(0x92, 32), 1'700'000'510, 1'700'000'511'000);
    ASSERT_TRUE(awaitIngestBatchForSource(io, store, source_id, {conn_event}, {conn_block}, 102, 1'700'000'511'100));
    node->addTransaction(conn_event.raw.tx_hash, conn_event.raw.block_hash);
    ASSERT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 101, 101, 1'700'000'512'000));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, {}, makeClient(node));
    EXPECT_EQ(runAw(io, projector.projectBatch(10, 1'700'000'513'000)), 1u);
    EXPECT_TRUE(runAw(io, registry.hasConnector("backfill_connector")));

    for(const auto kind : {registry::EntityKind::TRANSFORMATION, registry::EntityKind::CONDITION})
    {
        const std::string name = kind == registry::EntityKind::TRANSFORMATION ? "early_tf" : "early_cond";
        const auto location = runAw(io, registry.getEntityLocation(kind, name));
        ASSERT_TRUE(location.has_value()) << name;
        EXPECT_EQ(location->source.source_id, source_id);
        EXPECT_EQ(location->source.content_hash, chain::bytes32ToHex(bytes32OfFill(0xAA)));
        EXPECT_EQ(location->source.metadata_hash, chain::bytes32ToHex(bytes32OfFill(0xBB)));
        EXPECT_EQ(location->source.runtime_code_hash, chain::bytes32ToHex(chain::keccakBytes(
            std::vector<std::uint8_t>{0x60, 0x00, 0x60, 0x00}.data(), 4)));
        EXPECT_EQ(location->address, makeAddressFromByte(0x55));

        const auto code = runAw(io, registry.getRuntimeCode(location->source.runtime_code_hash));
        ASSERT_TRUE(code.has_value()) << name;
        EXPECT_EQ(*code, (std::vector<std::uint8_t>{0x60, 0x00, 0x60, 0x00}));
    }
    const auto transformation = runAw(io, registry.getTransformationRecordHandle("early_tf"));
    ASSERT_TRUE(transformation.has_value());
    EXPECT_EQ((**transformation).transformation().args_count(), 2u);
    EXPECT_EQ(chain::normalizeHex((**transformation).owner()), hexAddress(0x42));

    // Registry state is read at the block of the connector's event.
    ASSERT_FALSE(node->calls.empty());
    for(const auto & call : node->calls)
    {
        EXPECT_EQ(call.at(1).at("blockHash"), conn_event.raw.block_hash);
    }

    // A dependency the chain cannot answer for leaves the connector unmaterialized.
    registry::Registry other_registry(io);
    const auto other_paths = makeTempEventsPaths("reg_proj_backfill_fail");
    events::SQLiteHotStore other_store(other_paths.hot_db, CHAIN_ID);
    ASSERT_TRUE(awaitIngestBatchForSource(io, other_store, source_id, {conn_event}, {conn_block}, 102, 1'700'000'511'100));
    ASSERT_TRUE(awaitApplyFinalityForSource(io, other_store, source_id, 200, 101, 101, 1'700'000'512'000));
    auto silent_node = std::make_shared<FakeNode>();
    registry::RegistryProjector failing(other_store, other_registry, write_strand, {}, makeClient(silent_node));
    runAw(io, failing.projectBatch(10, 1'700'000'513'000));
    EXPECT_FALSE(runAw(io, other_registry.hasConnector("backfill_connector")));
}

TEST(RegistryProjection, ChainTransformation_StoresItsRuntimeCodeReadFromTheChain)
{
    using namespace dcn::tests::fake_eth;

    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);
    const std::vector<std::uint8_t> code{0x60, 0x00, 0x60, 0x00};
    const std::string code_hash = chain::bytes32ToHex(chain::keccakBytes(code.data(), code.size()));
    auto node = std::make_shared<FakeNode>();
    node->code = chain::bytesToHex(code);
    node->addTransaction(hexBytes(0xBB, 32), hexBytes(0xAA, 32));

    asio::io_context io;
    const auto write_strand = asio::make_strand(io);

    // Projects one chain TransformationAdded whose event carries `runtime_code_hash` and
    // answers whether the transformation was materialized.
    const auto project = [&](const std::string & label, registry::Registry & registry, const std::string & runtime_code_hash)
    {
        const auto paths = makeTempEventsPaths(label);
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

        events::DecodedEvent event = makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
        event.raw.source_id = source_id;
        event.raw.address = REGISTRY_ADDRESS;
        event.raw.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41), "coded_tf", makeAddressFromByte(0x43), makeAddressFromByte(0x42), 0);
        event.name = "coded_tf";
        event.owner = hexAddress(0x42);
        event.runtime_code_hash = runtime_code_hash;

        const events::ChainBlockInfo block =
            makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
        EXPECT_TRUE(awaitIngestBatchForSource(io, store, source_id, {event}, {block}, 101, 1'700'000'501'100));
        EXPECT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 100, 100, 1'700'000'502'000));

        registry::RegistryProjector projector(store, registry, write_strand, {}, makeClient(node));
        runAw(io, projector.projectBatch(10, 1'700'000'503'000));
        return runAw(io, registry.hasTransformation("coded_tf"));
    };

    registry::Registry registry(io);
    EXPECT_TRUE(project("reg_proj_runtime_code", registry, code_hash));
    const auto stored = runAw(io, registry.getRuntimeCode(code_hash));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, code);

    // The code is read at exactly the block of the event.
    ASSERT_EQ(node->code_requests.size(), 1u);
    EXPECT_EQ(node->code_requests.front().at(1).at("blockHash"), hexBytes(0xAA, 32));

    // Code the registry already holds is not read again.
    EXPECT_TRUE(project("reg_proj_runtime_code_again", registry, code_hash));
    EXPECT_EQ(node->code_requests.size(), 1u);

    // Code that does not hash to the hash the registry event carries is never stored, and
    // the entity waits for it.
    registry::Registry mismatched(io);
    const std::string other_hash = chain::bytes32ToHex(bytes32OfFill(0xEE));
    EXPECT_FALSE(project("reg_proj_runtime_code_mismatch", mismatched, other_hash));
    EXPECT_FALSE(runAw(io, mismatched.getRuntimeCode(other_hash)).has_value());
}

// The creation code a chain entity was deployed from is read from its event's transaction,
// whether the transaction called the registry directly or a relay embeds the call; calldata
// without the publish call stores none, and an unknown transaction leaves the row incomplete.
TEST(RegistryProjection, ChainTransformation_StoresTheCreationCodeItsTransactionCarries)
{
    using namespace dcn::tests::fake_eth;

    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);
    const std::vector<std::uint8_t> runtime_code{0x60, 0x00, 0x60, 0x00};
    const std::vector<std::uint8_t> creation_code{0x60, 0x80, 0x60, 0x40, 0x52};

    pt::Publication publication;
    publication.kind = pt::EntityKind::TRANSFORMATION;
    publication.name = "published_tf";
    publication.owner = makeAddressFromByte(0x42);
    publication.content_hash = bytes32OfFill(0xC0);
    publication.metadata_hash = bytes32OfFill(0xC1);
    publication.creation_code_hash = chain::keccakBytes(creation_code);
    publication.runtime_code_hash = chain::keccakBytes(runtime_code);
    const chain::Address entity = pt::predictEntityAddress(
        *evmc::from_hex<chain::Address>(REGISTRY_ADDRESS), publication, creation_code);
    const std::vector<std::uint8_t> publish_call = pt::encodePublishTransformationCall(creation_code, publication, {});

    // A relay call carrying the publish call as a bytes argument.
    std::vector<std::uint8_t> relay_call = chain::constructSelector("execute(address,bytes)");
    const std::vector<std::uint8_t> relay_arguments = pt::abi::encodeTuple({
        pt::abi::addressComponent(*evmc::from_hex<chain::Address>(REGISTRY_ADDRESS)),
        pt::abi::bytesComponent(publish_call)});
    relay_call.insert(relay_call.end(), relay_arguments.begin(), relay_arguments.end());

    asio::io_context io;
    const auto write_strand = asio::make_strand(io);

    // Projects the event with a transaction carrying `input`, or with no transaction known.
    const auto project = [&](const std::string & label, const std::optional<std::vector<std::uint8_t>> & input)
    {
        auto node = std::make_shared<FakeNode>();
        node->code = chain::bytesToHex(runtime_code);

        events::DecodedEvent event = makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
        event.raw.source_id = source_id;
        event.raw.address = REGISTRY_ADDRESS;
        event.raw.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41), publication.name, entity, publication.owner, 0);
        event.name = publication.name;
        event.owner = hexAddress(0x42);
        event.entity_address = chain::addressToHex(entity);
        event.content_hash = chain::bytes32ToHex(publication.content_hash);
        event.runtime_code_hash = chain::bytes32ToHex(publication.runtime_code_hash);
        if(input) node->addTransaction(event.raw.tx_hash, event.raw.block_hash, *input);

        const auto paths = makeTempEventsPaths(label);
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
        const events::ChainBlockInfo block =
            makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
        EXPECT_TRUE(awaitIngestBatchForSource(io, store, source_id, {event}, {block}, 101, 1'700'000'501'100));
        EXPECT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 100, 100, 1'700'000'502'000));

        auto registry = std::make_unique<registry::Registry>(io);
        registry::RegistryProjector projector(store, *registry, write_strand, {}, makeClient(node));
        runAw(io, projector.projectBatch(10, 1'700'000'503'000));
        return registry;
    };

    for(const auto & [label, input] : {
        std::pair<std::string, std::vector<std::uint8_t>>{"reg_proj_creation_direct", publish_call},
        std::pair<std::string, std::vector<std::uint8_t>>{"reg_proj_creation_relayed", relay_call}})
    {
        SCOPED_TRACE(label);
        const auto registry = project(label, input);
        EXPECT_TRUE(runAw(io, registry->hasTransformation(publication.name)));
        EXPECT_EQ(runAw(io, registry->getChainCreationCode(source_id, entity)),
            std::optional<std::vector<std::uint8_t>>(creation_code));
    }

    // Calldata without the publish call: the entity is projected, without creation code.
    const auto without_call = project("reg_proj_creation_absent", std::vector<std::uint8_t>{0x12, 0x34, 0x56, 0x78});
    EXPECT_TRUE(runAw(io, without_call->hasTransformation(publication.name)));
    EXPECT_FALSE(runAw(io, without_call->hasChainCreationCode(source_id, entity)));

    // An unknown transaction leaves the projection incomplete: the row is retried.
    const auto unknown = project("reg_proj_creation_unknown_tx", std::nullopt);
    EXPECT_FALSE(runAw(io, unknown->hasTransformation(publication.name)));
}

// A batch reads the runtime code of all its chain transformations up front, each code once;
// rows still materialize in event order, and code the batch could not read is not read a
// second time when its row comes up.
TEST(RegistryProjection, ChainTransformations_ReadEachRuntimeCodeOncePerBatch)
{
    using namespace dcn::tests::fake_eth;

    const auto paths = makeTempEventsPaths("reg_proj_runtime_code_prefetch");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);
    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);
    auto node = std::make_shared<FakeNode>();

    // Twelve readable entities, then one whose code the chain does not return.
    constexpr std::uint8_t READABLE = 12;
    std::vector<events::DecodedEvent> events_in_block;
    for(std::uint8_t i = 0; i <= READABLE; ++i)
    {
        const std::uint8_t address_byte = static_cast<std::uint8_t>(0x50 + i);
        const std::vector<std::uint8_t> code{0x60, i, 0x60, 0x00};
        node->code_by_address[hexAddress(address_byte)] = i < READABLE ? chain::bytesToHex(code) : "0x";

        events::DecodedEvent event = makeDecodedEvent(
            100, i, i, 0xAA, static_cast<std::uint8_t>(0xB0 + i), events::EventType::TRANSFORMATION_ADDED);
        event.raw.source_id = source_id;
        event.raw.address = REGISTRY_ADDRESS;
        event.name = std::format("prefetched_tf_{}", i);
        event.raw.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41), event.name, makeAddressFromByte(address_byte), makeAddressFromByte(0x42), 0);
        event.owner = hexAddress(0x42);
        event.runtime_code_hash = chain::bytes32ToHex(chain::keccakBytes(code));
        node->addTransaction(event.raw.tx_hash, event.raw.block_hash);
        events_in_block.push_back(std::move(event));
    }

    const events::ChainBlockInfo block =
        makeBlockInfo(100, events_in_block.front().raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatchForSource(io, store, source_id, events_in_block, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 100, 100, 1'700'000'502'000));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, {}, makeClient(node));
    runAw(io, projector.projectBatch(256, 1'700'000'503'000));

    for(std::uint8_t i = 0; i < READABLE; ++i)
    {
        EXPECT_TRUE(runAw(io, registry.hasTransformation(std::format("prefetched_tf_{}", i)))) << int{i};
    }
    EXPECT_FALSE(runAw(io, registry.hasTransformation(std::format("prefetched_tf_{}", READABLE))));
    EXPECT_EQ(node->code_requests.size(), std::size_t{READABLE} + 1);
}

// A chain transformation whose runtime code cannot be read is an incomplete projection: the
// row is dead-lettered rather than materialized without its code, and the dead-letter sweep
// reads the code again over RPC and materializes it once the read succeeds.
TEST(RegistryProjection, ChainTransformation_WithUnreadableRuntimeCode_IsCompletedByTheSweep)
{
    using namespace dcn::tests::fake_eth;

    const auto paths = makeTempEventsPaths("reg_proj_runtime_code_sweep");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);
    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);

    const std::vector<std::uint8_t> code{0x60, 0x00, 0x60, 0x00};
    const std::string code_hash = chain::bytes32ToHex(chain::keccakBytes(code.data(), code.size()));
    auto node = std::make_shared<FakeNode>();
    node->code = "0x";
    node->addTransaction(hexBytes(0xBB, 32), hexBytes(0xAA, 32));

    events::DecodedEvent event = makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    event.raw.source_id = source_id;
    event.raw.address = REGISTRY_ADDRESS;
    event.raw.data_hex = encodeSimpleAddedEventDataV2(
        makeAddressFromByte(0x41), "late_code_tf", makeAddressFromByte(0x43), makeAddressFromByte(0x42), 0);
    event.name = "late_code_tf";
    event.owner = hexAddress(0x42);
    event.runtime_code_hash = code_hash;

    const events::ChainBlockInfo block =
        makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatchForSource(io, store, source_id, {event}, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 100, 100, 1'700'000'502'000));

    const events::ProjectorRetryConfig retry{.max_attempts = 1, .sweep_interval_ms = 1'000};
    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, retry, makeClient(node));

    // The code cannot be read: the row is dead-lettered, nothing is materialized.
    runAw(io, projector.projectBatch(10, 1'700'000'503'000));
    EXPECT_FALSE(runAw(io, registry.hasTransformation("late_code_tf")));
    EXPECT_EQ(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).size(), 1u);

    // Once the chain answers, the sweep completes the projection.
    node->code = chain::bytesToHex(code);
    runAw(io, projector.projectBatch(10, 1'700'000'503'000 + retry.sweep_interval_ms));
    EXPECT_TRUE(runAw(io, registry.hasTransformation("late_code_tf")));
    EXPECT_EQ(runAw(io, registry.getRuntimeCode(code_hash)), std::optional<std::vector<std::uint8_t>>(code));
    EXPECT_TRUE(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).empty());
}

// A local entity redeployed with other code, as after a compiler upgrade, keeps its
// definition; its local version takes the hashes of the newer event.
TEST(RegistryProjection, LocalTransformation_RebuiltWithOtherCodeTakesTheNewHashes)
{
    asio::io_context io;
    const auto write_strand = asio::make_strand(io);
    registry::Registry registry(io);

    const auto project = [&](const std::string & label, const std::string & seed)
    {
        events::DecodedEvent event = makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
        event.raw.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41), "rebuilt_tf", makeAddressFromByte(0x43), makeAddressFromByte(0x42), 0);
        event.name = "rebuilt_tf";
        event.owner = hexAddress(0x42);
        event.content_hash = chain::bytes32ToHex(chain::keccakString(seed + " content"));
        event.metadata_hash = chain::bytes32ToHex(chain::keccakString(seed + " metadata"));
        event.runtime_code_hash = chain::bytes32ToHex(chain::keccakString(seed + " runtime"));

        const auto paths = makeTempEventsPaths(label);
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
        const events::ChainBlockInfo block =
            makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
        EXPECT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {event}, {block}, 101, 1'700'000'501'100));
        EXPECT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 100, 100, 1'700'000'502'000));

        registry::RegistryProjector projector(store, registry, write_strand);
        EXPECT_GE(runAw(io, projector.projectBatch(10, 1'700'000'503'000)), 1u);
        return *event.runtime_code_hash;
    };

    const std::string old_runtime = project("reg_proj_local_old_build", "old");
    auto location = runAw(io, registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, "rebuilt_tf"));
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->source.source_id, "local");
    EXPECT_EQ(location->source.runtime_code_hash, old_runtime);

    const std::string new_runtime = project("reg_proj_local_new_build", "new");
    location = runAw(io, registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, "rebuilt_tf"));
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->source.runtime_code_hash, new_runtime);
    EXPECT_EQ(location->source.content_hash, chain::bytes32ToHex(chain::keccakString("new content")));
}

// ---------------------------------------------------------------------------
// Test 4: A finalized row that cannot be materialized (undecodable payload) is
//         retried for the configured retry budget and then dead-lettered, so the
//         cursor — and with it the prune watermark shared by every projector —
//         is never wedged, later rows still materialize, and the quarantined row
//         is exempt from pruning (no chain event data is lost).
// ---------------------------------------------------------------------------
TEST(RegistryProjection, PoisonRow_IsRetriedThenDeadLettered_AndDoesNotWedgePipeline)
{
    const auto paths = makeTempEventsPaths("reg_proj_poison");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    // Poison event: makeDecodedEvent's default data_hex ("0x00") is not a valid
    // TransformationAdded ABI payload, so _materializeOne fails to decode it.
    events::DecodedEvent poison_event =
        makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    poison_event.name = "poison_tf";

    // Good event behind it: blocked while the poison row parks the cursor.
    const chain::Address owner     = makeAddressFromByte(0x42);
    const chain::Address tf_addr   = makeAddressFromByte(0x43);
    const std::string    good_name = "unwedged_tf";

    events::DecodedEvent good_event =
        makeDecodedEvent(101, 0, 1, 0xCC, 0xDD, events::EventType::TRANSFORMATION_ADDED);
    good_event.raw.data_hex =
        encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), good_name, tf_addr, owner, 0);
    good_event.name  = good_name;
    good_event.owner = chain::addressToHex(owner);

    const events::ChainBlockInfo block_a =
        makeBlockInfo(100, poison_event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
    const events::ChainBlockInfo block_b =
        makeBlockInfo(101, good_event.raw.block_hash, hexBytes(0x92, 32), 1'700'000'510, 1'700'000'511'000);

    ASSERT_TRUE(awaitIngestBatch(
        io, store, CHAIN_ID, {poison_event, good_event}, {block_a, block_b}, 102, 1'700'000'511'100));

    const events::ProjectorRetryConfig retry_cfg{};
    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, retry_cfg);

    // Drain the observed rows (skipped, cursor advances past them).
    EXPECT_EQ(runAw(io, projector.projectBatch(10, 1'700'000'512'000)), 2u);
    const std::int64_t cursor_after_observed = projector.cursor();
    EXPECT_GT(cursor_after_observed, 0);

    // Finalize both rows so they re-surface with higher change_seqs.
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 101, 101, 1'700'000'513'000));

    // Failing passes 1..MAX-1: the poison row parks the cursor; nothing is consumed
    // and the good row behind it stays blocked.
    for(std::size_t attempt = 1; attempt < retry_cfg.max_attempts; ++attempt)
    {
        EXPECT_EQ(runAw(io, projector.projectBatch(10, 1'700'000'514'000)), 0u) << "attempt " << attempt;
        EXPECT_EQ(projector.cursor(), cursor_after_observed) << "attempt " << attempt;
        EXPECT_FALSE(runAw(io, registry.hasTransformation(good_name))) << "attempt " << attempt;
    }

    // Final attempt: retry budget exhausted → poison row dead-lettered and skipped,
    // good row materializes, cursor (prune watermark) advances past both.
    EXPECT_EQ(runAw(io, projector.projectBatch(10, 1'700'000'515'000)), 2u);
    EXPECT_TRUE(runAw(io, registry.hasTransformation(good_name)));
    EXPECT_FALSE(runAw(io, registry.hasTransformation("poison_tf")));
    EXPECT_GT(projector.cursor(), cursor_after_observed);

    // The poison row is quarantined, not lost: it is visible via readDeadLetters
    // and exempt from pruning even though the cursor has moved past it.
    {
        const auto dead = store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10);
        ASSERT_EQ(dead.size(), 1u);
        EXPECT_EQ(dead.front().block_number, 100);
    }

    (void)store.pruneConsumedRaw("local", projector.cursor(), 1'000, 100);
    EXPECT_EQ(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).size(), 1u);

    // The idle-time sweep retries the row; the payload is still undecodable, so it
    // stays dead-lettered (retained) rather than being dropped.
    EXPECT_EQ(runAw(io, projector.projectBatch(
        10, 1'700'000'515'000 + retry_cfg.sweep_interval_ms)), 0u);
    EXPECT_EQ(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).size(), 1u);
}

// ---------------------------------------------------------------------------
// Test 5: A dead-lettered row whose failure cause goes away is recovered by the
//         idle-time sweep: the event materializes into the registry, the
//         dead-letter bit clears, and the row becomes prunable again.
// ---------------------------------------------------------------------------
TEST(RegistryProjection, DeadLetteredRow_IsRecoveredBySweep_AndBecomesPrunable)
{
    const auto paths = makeTempEventsPaths("reg_proj_dead_letter_recover");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    const chain::Address owner   = makeAddressFromByte(0x42);
    const chain::Address tf_addr = makeAddressFromByte(0x43);
    const std::string    name    = "dead_letter_recovered_tf";

    events::DecodedEvent event =
        makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    event.raw.data_hex =
        encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), name, tf_addr, owner, 0);
    event.name  = name;
    event.owner = chain::addressToHex(owner);

    const events::ChainBlockInfo block =
        makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {event}, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 100, 100, 1'700'000'502'000));

    // Quarantined history: the row is dead-lettered and the cursor is already past
    // it, exactly the state an exhausted retry budget leaves behind.
    const auto changes = store.readChangesSince(0, 10);
    ASSERT_EQ(changes.size(), 1u);
    ASSERT_TRUE(store.markDeadLetter(
        events::REGISTRY_DEAD_LETTER_BIT, changes.front().source_id,
        changes.front().block_hash, changes.front().log_index));
    ASSERT_TRUE(runAw(io, registry.setMaterializationCursor(changes.front().change_seq)));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand);
    EXPECT_EQ(projector.cursor(), changes.front().change_seq);

    // While dead-lettered, the row is exempt from pruning.
    EXPECT_EQ(store.pruneConsumedRaw("local", projector.cursor(), 1'000, 100), 0u);
    EXPECT_FALSE(runAw(io, registry.hasTransformation(name)));

    // Idle pass → the sweep recovers the event into the registry and clears the bit.
    EXPECT_EQ(runAw(io, projector.projectBatch(10, 1'700'000'503'000)), 1u);
    EXPECT_TRUE(runAw(io, registry.hasTransformation(name)));
    EXPECT_TRUE(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).empty());

    // Once resolved, the row becomes prunable again.
    EXPECT_EQ(store.pruneConsumedRaw("local", projector.cursor(), 1'000, 100), 1u);
}

// ---------------------------------------------------------------------------
// Test 6: The retry budget is configurable: with max_attempts=2 a failing row
//         parks the cursor for exactly one pass and is dead-lettered on the
//         second, instead of the default five.
// ---------------------------------------------------------------------------
TEST(RegistryProjection, RetryBudget_IsConfigurable)
{
    const auto paths = makeTempEventsPaths("reg_proj_retry_budget");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    // makeDecodedEvent's default data_hex ("0x00") is not a valid ABI payload,
    // so _materializeOne fails to decode it on every pass.
    events::DecodedEvent poison_event =
        makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    poison_event.name = "budget_poison_tf";

    const events::ChainBlockInfo block =
        makeBlockInfo(100, poison_event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {poison_event}, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 100, 100, 1'700'000'502'000));

    const events::ProjectorRetryConfig retry_cfg{.max_attempts = 2};
    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, retry_cfg);

    const std::int64_t cursor_before = projector.cursor();

    // Pass 1: failure parks the cursor; the row is not yet quarantined.
    EXPECT_EQ(runAw(io, projector.projectBatch(10, 1'700'000'503'000)), 0u);
    EXPECT_EQ(projector.cursor(), cursor_before);
    EXPECT_TRUE(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).empty());

    // Pass 2: budget exhausted → dead-lettered, cursor advances past the row.
    EXPECT_EQ(runAw(io, projector.projectBatch(10, 1'700'000'504'000)), 1u);
    EXPECT_GT(projector.cursor(), cursor_before);
    EXPECT_EQ(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).size(), 1u);
}

// ---------------------------------------------------------------------------
// Test 7: The sweep interval is configurable: an idle pass inside the interval
//         does not retry dead letters, one at the interval boundary does.
// ---------------------------------------------------------------------------
TEST(RegistryProjection, DeadLetterSweepInterval_IsConfigurable)
{
    const auto paths = makeTempEventsPaths("reg_proj_sweep_interval");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    const chain::Address owner   = makeAddressFromByte(0x42);
    const chain::Address tf_addr = makeAddressFromByte(0x43);
    const std::string    name    = "sweep_interval_tf";

    events::DecodedEvent event =
        makeDecodedEvent(100, 0, 1, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
    event.raw.data_hex =
        encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), name, tf_addr, owner, 0);
    event.name  = name;
    event.owner = chain::addressToHex(owner);

    const events::ChainBlockInfo block =
        makeBlockInfo(100, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {event}, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 200, 100, 100, 1'700'000'502'000));

    const auto changes = store.readChangesSince(0, 10);
    ASSERT_EQ(changes.size(), 1u);
    ASSERT_TRUE(runAw(io, registry.setMaterializationCursor(changes.front().change_seq)));

    const events::ProjectorRetryConfig retry_cfg{.sweep_interval_ms = 10'000};
    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, retry_cfg);

    // First idle pass at T: nothing is dead-lettered yet, but the sweep runs and
    // anchors its timestamp at T.
    const std::int64_t t0 = 1'700'000'600'000;
    EXPECT_EQ(runAw(io, projector.projectBatch(10, t0)), 0u);

    // Quarantine the (recoverable) row after the anchor pass.
    ASSERT_TRUE(store.markDeadLetter(
        events::REGISTRY_DEAD_LETTER_BIT, changes.front().source_id,
        changes.front().block_hash, changes.front().log_index));

    // Inside the interval: the sweep is throttled, so the row is not retried.
    EXPECT_EQ(runAw(io, projector.projectBatch(10, t0 + retry_cfg.sweep_interval_ms - 1)), 0u);
    EXPECT_FALSE(runAw(io, registry.hasTransformation(name)));
    EXPECT_EQ(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).size(), 1u);

    // At the interval boundary: the sweep runs, recovers the row, clears the bit.
    EXPECT_EQ(runAw(io, projector.projectBatch(10, t0 + retry_cfg.sweep_interval_ms)), 1u);
    EXPECT_TRUE(runAw(io, registry.hasTransformation(name)));
    EXPECT_TRUE(store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, 10).empty());
}

// Entities a multicall publishes in one transaction have their creation code read from a
// single lookup of that transaction.
TEST(RegistryProjection, ChainTransformations_ReadTheirSharedPublicationTransactionOnce)
{
    using namespace dcn::tests::fake_eth;

    const auto paths = makeTempEventsPaths("reg_proj_creation_multicall");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);
    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);
    const std::vector<std::uint8_t> runtime_code{0x60, 0x00, 0x60, 0x00};
    auto node = std::make_shared<FakeNode>();
    node->code = chain::bytesToHex(runtime_code);

    std::vector<ChainPublication> publications;
    std::vector<pt::abi::Component> calls;
    std::vector<events::DecodedEvent> events_in_tx;
    for(std::uint8_t i = 0; i < 3; ++i)
    {
        publications.push_back(makeChainPublication(
            std::format("batched_tf_{}", i), i, bytes32OfFill(0xC0), chain::keccakBytes(runtime_code)));
        const ChainPublication & published = publications.back();
        calls.push_back(pt::abi::bytesComponent(published.call));

        events::DecodedEvent event = makeDecodedEvent(100, 0, i, 0xAA, 0xBB, events::EventType::TRANSFORMATION_ADDED);
        event.raw.source_id = source_id;
        event.raw.address = REGISTRY_ADDRESS;
        event.raw.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41), published.publication.name, published.entity, published.publication.owner, 0);
        event.name = published.publication.name;
        event.owner = hexAddress(0x42);
        event.entity_address = chain::addressToHex(published.entity);
        event.content_hash = chain::bytes32ToHex(published.publication.content_hash);
        event.runtime_code_hash = chain::bytes32ToHex(published.publication.runtime_code_hash);
        events_in_tx.push_back(std::move(event));
    }
    std::vector<std::uint8_t> multicall = chain::constructSelector("multicall(bytes,bytes,bytes)");
    const std::vector<std::uint8_t> arguments = pt::abi::encodeTuple(calls);
    multicall.insert(multicall.end(), arguments.begin(), arguments.end());
    node->addTransaction(events_in_tx.front().raw.tx_hash, events_in_tx.front().raw.block_hash, multicall);

    const events::ChainBlockInfo block =
        makeBlockInfo(100, events_in_tx.front().raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatchForSource(io, store, source_id, events_in_tx, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 100, 100, 1'700'000'502'000));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, {}, makeClient(node));
    runAw(io, projector.projectBatch(10, 1'700'000'503'000));

    for(const auto & published : publications)
    {
        EXPECT_TRUE(runAw(io, registry.hasTransformation(published.publication.name))) << published.publication.name;
        EXPECT_EQ(runAw(io, registry.getChainCreationCode(source_id, published.entity)),
            std::optional<std::vector<std::uint8_t>>(published.creation_code)) << published.publication.name;
    }
    EXPECT_EQ(node->transaction_requests.size(), 1u);
}

// A failing provider stops the batch's up-front reads at the first failure of each worker
// instead of holding the batch until every transaction of it has failed.
TEST(RegistryProjection, ChainTransformations_StopPrefetchingAfterAFailedRead)
{
    using namespace dcn::tests::fake_eth;

    const auto paths = makeTempEventsPaths("reg_proj_prefetch_stop");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);
    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);
    const std::vector<std::uint8_t> runtime_code{0x60, 0x00, 0x60, 0x00};
    auto node = std::make_shared<FakeNode>();
    node->code = chain::bytesToHex(runtime_code);

    // Twenty entities, each in its own transaction, none of which the node knows.
    constexpr std::uint8_t ENTITIES = 20;
    std::vector<events::DecodedEvent> events_in_block;
    for(std::uint8_t i = 0; i < ENTITIES; ++i)
    {
        events::DecodedEvent event = makeDecodedEvent(
            100, i, i, 0xAA, static_cast<std::uint8_t>(0xB0 + i), events::EventType::TRANSFORMATION_ADDED);
        event.raw.source_id = source_id;
        event.raw.address = REGISTRY_ADDRESS;
        event.name = std::format("unreadable_tf_{}", i);
        event.raw.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41), event.name, makeAddressFromByte(static_cast<std::uint8_t>(0x50 + i)),
            makeAddressFromByte(0x42), 0);
        event.owner = hexAddress(0x42);
        event.runtime_code_hash = chain::bytes32ToHex(chain::keccakBytes(runtime_code));
        events_in_block.push_back(std::move(event));
    }

    const events::ChainBlockInfo block =
        makeBlockInfo(100, events_in_block.front().raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatchForSource(io, store, source_id, events_in_block, {block}, 101, 1'700'000'501'100));
    ASSERT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 100, 100, 1'700'000'502'000));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, {}, makeClient(node));
    runAw(io, projector.projectBatch(256, 1'700'000'503'000));

    EXPECT_FALSE(runAw(io, registry.hasTransformation("unreadable_tf_0")));
    // At most one failed lookup per worker, and the first row reuses its failed read.
    EXPECT_GE(node->transaction_requests.size(), 1u);
    EXPECT_LE(node->transaction_requests.size(), 8u);
}

// A back-filled dependency whose creation code a transient provider failure keeps from
// being read is not stored without it: the connector's row fails, and its retry recovers
// the dependency together with its creation code.
TEST(RegistryProjection, ChainConnector_RetriesABackfilledCreationCodeAfterATransientFailure)
{
    using namespace dcn::tests::fake_eth;

    const auto paths = makeTempEventsPaths("reg_proj_backfill_retry");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);
    const std::string source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);

    const ChainPublication early = makeChainPublication("early_tf", 0x01, bytes32OfFill(0xAA), bytes32OfFill(0xDD));
    auto node = std::make_shared<FakeNode>();
    node->entity_address = wordOfAddress(chain::addressToHex(early.entity));
    node->call_override = [](const json & request) -> std::string
    {
        const std::string data = request.at("params").at(0).value("data", "");
        if(data.starts_with(selectorOf("getEntityMetadataHash(uint8,string)"))) return rpcResult(request, wordOfFill(0xBB));
        if(data.starts_with(selectorOf("getArgsCount()"))) return rpcResult(request, wordOfUint(2));
        return {};
    };

    // The dependency was published in block 50, by a transaction carrying its publish call.
    node->code_created_at[chain::addressToHex(early.entity)] = 50;
    node->logs_by_block_number[50] = json::array({json{
        {"address", REGISTRY_ADDRESS},
        {"topics", json::array({pt::eventTopic(pt::TRANSFORMATION_ADDED_SIGNATURE)})},
        {"data", encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), "early_tf", early.entity, makeAddressFromByte(0x42), 0)},
        {"transactionHash", hexBytes(0x07, 32)},
        {"blockHash", hexBytes(0x50, 32)}}});
    node->addTransaction(hexBytes(0x07, 32), hexBytes(0x50, 32), early.call);

    // The publication search reads code by block number; those reads fail while the
    // provider is down.
    bool provider_down = true;
    node->transport_fault = [&provider_down](const json & request) -> std::optional<rpc::RpcError>
    {
        const json params = request.value("params", json::array());
        if(provider_down && request.value("method", "") == "eth_getCode" && params.size() > 1 && params.at(1).is_string())
        {
            return rpc::RpcError{.kind = rpc::RpcError::Kind::TRANSPORT, .message = "connection reset"};
        }
        return std::nullopt;
    };

    ConnectorRecord conn_record;
    conn_record.set_owner(hexAddress(0x42));
    conn_record.mutable_connector()->set_name("backfill_retry_connector");
    conn_record.mutable_connector()->add_dimensions()->add_transformations()->set_name("early_tf");
    const auto enc = dcn::tests::encodeConnectorAddedForTest(conn_record);

    events::DecodedEvent conn_event =
        makeDecodedEvent(101, 0, 1, 0xCC, 0xDD, events::EventType::CONNECTOR_ADDED);
    conn_event.raw.source_id = source_id;
    conn_event.raw.address = REGISTRY_ADDRESS;
    conn_event.raw.data_hex = enc.data_hex;
    conn_event.raw.topics[0] = enc.topics_hex[0];
    conn_event.raw.topics[1] = enc.topics_hex[1];
    conn_event.raw.topics[2] = enc.topics_hex[2];
    conn_event.name = "backfill_retry_connector";

    const events::ChainBlockInfo conn_block =
        makeBlockInfo(101, conn_event.raw.block_hash, hexBytes(0x92, 32), 1'700'000'510, 1'700'000'511'000);
    ASSERT_TRUE(awaitIngestBatchForSource(io, store, source_id, {conn_event}, {conn_block}, 102, 1'700'000'511'100));
    node->addTransaction(conn_event.raw.tx_hash, conn_event.raw.block_hash);
    ASSERT_TRUE(awaitApplyFinalityForSource(io, store, source_id, 200, 101, 101, 1'700'000'512'000));

    const auto write_strand = asio::make_strand(io);
    registry::RegistryProjector projector(store, registry, write_strand, {}, makeClient(node));
    runAw(io, projector.projectBatch(10, 1'700'000'513'000));
    EXPECT_FALSE(runAw(io, registry.hasConnector("backfill_retry_connector")));
    EXPECT_FALSE(runAw(io, registry.hasTransformation("early_tf")));

    provider_down = false;
    runAw(io, projector.projectBatch(10, 1'700'000'514'000));
    EXPECT_TRUE(runAw(io, registry.hasConnector("backfill_retry_connector")));
    EXPECT_TRUE(runAw(io, registry.hasTransformation("early_tf")));
    EXPECT_EQ(runAw(io, registry.getChainCreationCode(source_id, early.entity)),
        std::optional<std::vector<std::uint8_t>>(early.creation_code));
}
