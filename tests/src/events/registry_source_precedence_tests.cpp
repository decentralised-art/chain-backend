#include "unit-tests.hpp"
#include "events_test_harness.hpp"

#include "registry_projector.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    // A canonical RPC source identity as produced by the design's recommended form
    // `eip155:<chain-id>:<lowercase-registry-address>`.
    const std::string REMOTE_SOURCE = "eip155:11155111:0x00000000000000000000000000000000000000ab";
    constexpr int REMOTE_CHAIN_ID = 11155111;

    template<class AwaitableT>
    auto runAw(asio::io_context & io, AwaitableT aw)
    {
        auto future = asio::co_spawn(io, std::move(aw), asio::use_future);
        io.restart();
        io.run();
        return future.get();
    }

    events::DecodedEvent makeRemoteEvent(
        const events::EventType type,
        const std::int64_t block_number,
        const std::int64_t log_index,
        const std::uint8_t block_hash_byte,
        const std::string & name,
        std::string data_hex,
        const std::string & topic_signature)
    {
        events::DecodedEvent event = makeDecodedEvent(
            block_number, 0, log_index, block_hash_byte, 0xBB, type);
        event.raw.source_id = REMOTE_SOURCE;
        event.raw.chain_namespace = "eth";
        event.raw.chain_id = REMOTE_CHAIN_ID;
        event.raw.topics[0] = topicForEvent(topic_signature);
        event.raw.data_hex = std::move(data_hex);
        event.name = name;
        return event;
    }

    TransformationRecord makeLocalTransformation(const std::string & name, const chain::Address & owner)
    {
        TransformationRecord record;
        record.set_owner(chain::addressToHex(owner));
        record.mutable_transformation()->set_name(name);
        record.mutable_transformation()->set_args_count(2);
        return record;
    }

    std::size_t projectAll(
        asio::io_context & io,
        events::SQLiteHotStore & store,
        registry::Registry & registry,
        const std::int64_t now_ms)
    {
        const auto write_strand = asio::make_strand(io);
        registry::RegistryProjector projector(store, registry, write_strand);
        return runAw(io, projector.projectBatch(64, now_ms));
    }
}

// A chain publication of a name that already exists locally must be indexed as its own
// version and take precedence for reads; the local version stays stored as the fallback.
// When the chain version is removed, the local version becomes effective again.
TEST(RegistrySourcePrecedence, RemoteVersion_TakesPrecedence_AndRemovalRestoresLocal)
{
    const auto paths = makeTempEventsPaths("reg_src_precedence");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    const std::string name = "shared_tf";
    const chain::Address local_owner = makeAddressFromByte(0x42);
    const chain::Address remote_owner = makeAddressFromByte(0x52);
    const chain::Address remote_entity = makeAddressFromByte(0x77);

    // Local simulation version: no chain address, so reads report the 0x0 sentinel.
    const registry::EntitySource local_source{
        .content_hash = chain::normalizeHex(hexBytes(0x11, 32))};
    ASSERT_TRUE(runAw(io, registry.addTransformationVersion(
        local_source, chain::Address{}, makeLocalTransformation(name, local_owner))));
    ASSERT_TRUE(runAw(io, registry.hasTransformation(name)));
    EXPECT_FALSE(runAw(io, registry.hasRemoteEntity(registry::EntityKind::TRANSFORMATION, name)));
    {
        const auto location = runAw(io, registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, name));
        ASSERT_TRUE(location.has_value());
        EXPECT_EQ(location->source.source_id, "local");
        EXPECT_EQ(location->address, chain::Address{});
    }

    // ---- chain publication of the same name ----
    events::DecodedEvent added = makeRemoteEvent(
        events::EventType::TRANSFORMATION_ADDED,
        100,
        0,
        0xAA,
        name,
        encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), name, remote_entity, remote_owner, 2, 0x31),
        "TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)");
    added.owner = chain::addressToHex(remote_owner);
    added.entity_address = chain::addressToHex(remote_entity);

    const events::ChainBlockInfo added_block =
        makeBlockInfo(100, added.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatchForSource(
        io, store, REMOTE_SOURCE, {added}, {added_block}, 101, 1'700'000'501'100, REMOTE_CHAIN_ID));
    ASSERT_TRUE(awaitApplyFinalityForSource(
        io, store, REMOTE_SOURCE, 200, 100, 100, 1'700'000'502'000, REMOTE_CHAIN_ID));
    EXPECT_EQ(projectAll(io, store, registry, 1'700'000'503'000), 1u);

    EXPECT_TRUE(runAw(io, registry.hasRemoteEntity(registry::EntityKind::TRANSFORMATION, name)));
    {
        const auto location = runAw(io, registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, name));
        ASSERT_TRUE(location.has_value());
        EXPECT_EQ(location->source.source_id, REMOTE_SOURCE);
        EXPECT_EQ(location->source.chain_id, REMOTE_CHAIN_ID);
        EXPECT_EQ(location->address, remote_entity);
        EXPECT_TRUE(location->source.definition_conflict);
    }

    // ---- chain removal of the remote version ----
    events::DecodedEvent removed = makeRemoteEvent(
        events::EventType::TRANSFORMATION_REMOVED,
        101,
        0,
        0xAC,
        name,
        encodeRemovedEventData(makeAddressFromByte(0x41), remote_owner, name, remote_entity, 0x31, 0x32),
        "TransformationRemoved(address,address,string,address,bytes32,bytes32)");
    removed.owner = chain::addressToHex(remote_owner);
    removed.entity_address = chain::addressToHex(remote_entity);

    const events::ChainBlockInfo removed_block =
        makeBlockInfo(101, removed.raw.block_hash, hexBytes(0x92, 32), 1'700'000'600, 1'700'000'601'000);

    ASSERT_TRUE(awaitIngestBatchForSource(
        io, store, REMOTE_SOURCE, {removed}, {removed_block}, 102, 1'700'000'601'100, REMOTE_CHAIN_ID));
    ASSERT_TRUE(awaitApplyFinalityForSource(
        io, store, REMOTE_SOURCE, 201, 101, 101, 1'700'000'602'000, REMOTE_CHAIN_ID));
    EXPECT_EQ(projectAll(io, store, registry, 1'700'000'603'000), 1u);

    // The remote name is no longer reserved and the retained local version is effective.
    EXPECT_FALSE(runAw(io, registry.hasRemoteEntity(registry::EntityKind::TRANSFORMATION, name)));
    EXPECT_TRUE(runAw(io, registry.hasTransformation(name)));
    {
        const auto location = runAw(io, registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, name));
        ASSERT_TRUE(location.has_value());
        EXPECT_EQ(location->source.source_id, "local");
        EXPECT_EQ(location->address, chain::Address{});
    }
}

// EntityOwnerChanged rewrites the owner of the version that emitted it.
TEST(RegistrySourcePrecedence, EntityOwnerChanged_UpdatesOwnerOfThatSourceVersion)
{
    const auto paths = makeTempEventsPaths("reg_src_owner_changed");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    const std::string name = "owned_tf";
    const chain::Address old_owner = makeAddressFromByte(0x52);
    const chain::Address new_owner = makeAddressFromByte(0x53);
    const chain::Address entity = makeAddressFromByte(0x78);

    events::DecodedEvent added = makeRemoteEvent(
        events::EventType::TRANSFORMATION_ADDED,
        100,
        0,
        0xAA,
        name,
        encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), name, entity, old_owner, 2),
        "TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)");
    added.owner = chain::addressToHex(old_owner);
    added.entity_address = chain::addressToHex(entity);

    const events::ChainBlockInfo added_block =
        makeBlockInfo(100, added.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatchForSource(
        io, store, REMOTE_SOURCE, {added}, {added_block}, 101, 1'700'000'501'100, REMOTE_CHAIN_ID));
    ASSERT_TRUE(awaitApplyFinalityForSource(
        io, store, REMOTE_SOURCE, 200, 100, 100, 1'700'000'502'000, REMOTE_CHAIN_ID));
    EXPECT_EQ(projectAll(io, store, registry, 1'700'000'503'000), 1u);

    events::DecodedEvent owner_changed = makeRemoteEvent(
        events::EventType::ENTITY_OWNER_CHANGED,
        101,
        0,
        0xAD,
        name,
        encodeOwnerChangedEventData(1, name, entity, old_owner, new_owner),
        "EntityOwnerChanged(uint8,string,address,address,address)");
    owner_changed.owner = chain::addressToHex(new_owner);
    owner_changed.entity_address = chain::addressToHex(entity);
    owner_changed.entity_kind = pt::EntityKind::TRANSFORMATION;
    owner_changed.old_owner = chain::addressToHex(old_owner);

    const events::ChainBlockInfo owner_block =
        makeBlockInfo(101, owner_changed.raw.block_hash, hexBytes(0x93, 32), 1'700'000'600, 1'700'000'601'000);

    ASSERT_TRUE(awaitIngestBatchForSource(
        io, store, REMOTE_SOURCE, {owner_changed}, {owner_block}, 102, 1'700'000'601'100, REMOTE_CHAIN_ID));
    ASSERT_TRUE(awaitApplyFinalityForSource(
        io, store, REMOTE_SOURCE, 201, 101, 101, 1'700'000'602'000, REMOTE_CHAIN_ID));
    EXPECT_EQ(projectAll(io, store, registry, 1'700'000'603'000), 1u);

    const auto handle = runAw(io, registry.getTransformationRecordHandle(name));
    ASSERT_TRUE(handle.has_value());
    ASSERT_TRUE(*handle);
    EXPECT_EQ(chain::normalizeHex((*handle)->owner()), chain::addressToHex(new_owner));

    const auto owned = runAw(io, registry.getOwnedTransformationsCursor(new_owner, std::nullopt, 16));
    EXPECT_NE(std::ranges::find(owned.entries, name), owned.entries.end());

    const auto previously_owned = runAw(io, registry.getOwnedTransformationsCursor(old_owner, std::nullopt, 16));
    EXPECT_EQ(std::ranges::find(previously_owned.entries, name), previously_owned.entries.end());
}

// Re-projecting the same chain event must not fail: entity versions are keyed by
// (source_id, name) and written with replace semantics.
TEST(RegistrySourcePrecedence, RemoteAdd_IsIdempotentAcrossRepeatedProjection)
{
    const auto paths = makeTempEventsPaths("reg_src_idempotent");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    registry::Registry registry(io);

    const std::string name = "idempotent_tf";
    const chain::Address owner = makeAddressFromByte(0x52);
    const chain::Address entity = makeAddressFromByte(0x79);

    events::DecodedEvent added = makeRemoteEvent(
        events::EventType::TRANSFORMATION_ADDED,
        100,
        0,
        0xAA,
        name,
        encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), name, entity, owner, 2),
        "TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)");
    added.owner = chain::addressToHex(owner);
    added.entity_address = chain::addressToHex(entity);

    const events::ChainBlockInfo block =
        makeBlockInfo(100, added.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);

    ASSERT_TRUE(awaitIngestBatchForSource(
        io, store, REMOTE_SOURCE, {added}, {block}, 101, 1'700'000'501'100, REMOTE_CHAIN_ID));
    ASSERT_TRUE(awaitApplyFinalityForSource(
        io, store, REMOTE_SOURCE, 200, 100, 100, 1'700'000'502'000, REMOTE_CHAIN_ID));
    EXPECT_EQ(projectAll(io, store, registry, 1'700'000'503'000), 1u);

    // A second registry instance replays the same changelog row from cursor 0.
    registry::Registry replayed_registry(io);
    EXPECT_EQ(projectAll(io, store, replayed_registry, 1'700'000'504'000), 1u);
    EXPECT_TRUE(runAw(io, replayed_registry.hasTransformation(name)));

    // And replaying it again against the already-populated registry still succeeds.
    ASSERT_TRUE(runAw(io, replayed_registry.setMaterializationCursor(0)));
    EXPECT_EQ(projectAll(io, store, replayed_registry, 1'700'000'505'000), 1u);

    const auto location = runAw(io, replayed_registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, name));
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->address, entity);
}
