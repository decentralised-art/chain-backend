#include "unit-tests.hpp"
#include "events_test_harness.hpp"
#include "events_sql_assertions.hpp"

#include "registry_projector.hpp"

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
