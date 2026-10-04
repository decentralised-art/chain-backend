#include "unit-tests.hpp"

#include "events_sql_assertions.hpp"
#include "events_test_harness.hpp"
#include "feed_projector.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

TEST_F(UnitTest, Events_Projector_TwoStage_IngestDoesNotPublishBeforeProjection)
{
    const auto paths = makeTempEventsPaths("project_two_stage");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'500);
    const events::ChainBlockInfo block = makeBlockInfo(60, event.raw.block_hash, hexBytes(0x91, 32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 61, 1'700'000'501'100));

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 0);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 0);
}

TEST_F(UnitTest, Events_Projector_ProjectBatch_WritesFeedAndOutboxAndClearsJobs)
{
    const auto paths = makeTempEventsPaths("project_batch");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(61, 1, 2, 0xB1, 0xD1, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'510);
    const events::ChainBlockInfo block = makeBlockInfo(61, event.raw.block_hash, hexBytes(0x90, 32), 1'700'000'510, 1'700'000'511'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 62, 1'700'000'511'100));

    EXPECT_EQ(awaitProjectBatch(store_io_context, store, feed_obj, 128, 1'700'000'511'200), 1u);
    EXPECT_EQ(awaitProjectBatch(store_io_context, store, feed_obj, 128, 1'700'000'511'300), 0u);

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 1);

    SqliteReadonly db(paths.feed_db);
    EXPECT_EQ(db.scalarText("SELECT status FROM feed_items_hot LIMIT 1;"), "observed");
    EXPECT_EQ(db.scalarInt64("SELECT visible FROM feed_items_hot LIMIT 1;"), 1);
    EXPECT_EQ(db.scalarText("SELECT op FROM global_outbox LIMIT 1;"), "insert");
    EXPECT_EQ(db.scalarText("SELECT status FROM global_outbox LIMIT 1;"), "observed");
}

TEST_F(UnitTest, Events_Projector_RepeatedRuns_AreIdempotentWithoutNewJobs)
{
    const auto paths = makeTempEventsPaths("project_idempotent");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(62, 1, 3, 0xB2, 0xD2, events::EventType::CONDITION_ADDED, events::EventState::OBSERVED, 1'700'000'520);
    const events::ChainBlockInfo block = makeBlockInfo(62, event.raw.block_hash, hexBytes(0x8F, 32), 1'700'000'520, 1'700'000'521'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 63, 1'700'000'521'100));

    EXPECT_EQ(awaitProjectBatch(store_io_context, store, feed_obj, 1, 1'700'000'521'200), 1u);
    EXPECT_EQ(awaitProjectBatch(store_io_context, store, feed_obj, 1, 1'700'000'521'300), 0u);
    EXPECT_EQ(awaitProjectBatch(store_io_context, store, feed_obj, 1, 1'700'000'521'400), 0u);

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 1);
}

TEST_F(UnitTest, Events_Projector_RequeuedNoopJobs_AreFullyDrainedAcrossBatches)
{
    const auto paths = makeTempEventsPaths("project_requeued_noop_jobs");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    std::vector<events::DecodedEvent> events_batch;
    std::vector<events::ChainBlockInfo> blocks_batch;
    events_batch.reserve(520);
    blocks_batch.reserve(520);
    for (int i = 0; i < 520; ++i)
    {
        const std::int64_t block_number = 1'000 + i;
        auto event = makeDecodedEvent(
            block_number,
            0,
            static_cast<std::int64_t>(i + 1),
            static_cast<std::uint8_t>(0x20 + (i % 200)),
            static_cast<std::uint8_t>(0x80 + (i % 120)),
            events::EventType::TRANSFORMATION_ADDED,
            events::EventState::OBSERVED,
            1'700'030'000 + i);
        events_batch.push_back(event);
        blocks_batch.push_back(makeBlockInfo(
            block_number,
            event.raw.block_hash,
            hexBytes(static_cast<std::uint8_t>(0x40 + (i % 180)), 32),
            1'700'030'000 + i,
            1'700'030'000'100 + i));
    }

    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        events_batch,
        blocks_batch,
        2'000,
        1'700'030'001'000));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'030'002'000), 520u);
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'030'003'000), 0u); // idempotent
}

TEST_F(UnitTest, Events_Projector_RestartAfterDurableIngest_ProjectsPendingJobs)
{
    const auto paths = makeTempEventsPaths("project_restart");

    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db);
        const events::DecodedEvent event = makeDecodedEvent(63, 2, 4, 0xB3, 0xD3, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'530);
        const events::ChainBlockInfo block = makeBlockInfo(63, event.raw.block_hash, hexBytes(0x8E, 32), 1'700'000'530, 1'700'000'531'000);
        ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 64, 1'700'000'531'100));
    }

    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db);
        feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);
        EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'532'000), 1u);
    }

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 1);
}

TEST_F(UnitTest, Events_Projector_FinalityTransition_EmitsUpdateOutboxItem)
{
    const auto paths = makeTempEventsPaths("project_finality_update");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(64, 0, 1, 0xB4, 0xD4, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'540);
    const events::ChainBlockInfo block = makeBlockInfo(64, event.raw.block_hash, hexBytes(0x8D, 32), 1'700'000'540, 1'700'000'541'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 65, 1'700'000'541'100));

    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'541'200), 1u);

    const events::FinalityHeights heights{
        .head = 100,
        .safe = 64,
        .finalized = 64
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, 1'700'000'542'000, 2048));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'542'100), 1u);

    events_sql::expectRowCount(paths.feed_db, "global_outbox", 2);

    SqliteReadonly fdb(paths.feed_db);
    EXPECT_EQ(fdb.scalarText("SELECT status FROM feed_items_hot LIMIT 1;"), "finalized");
    EXPECT_EQ(fdb.scalarText("SELECT op FROM global_outbox WHERE stream_seq=1;"), "insert");
    EXPECT_EQ(fdb.scalarText("SELECT op FROM global_outbox WHERE stream_seq=2;"), "update");
    EXPECT_EQ(fdb.scalarText("SELECT status FROM global_outbox WHERE stream_seq=2;"), "finalized");

    // Changelog-based projection tracks progress via feed_cursor in the feed DB; the events hot DB
    // has no projection-version column (it was vestigial after feed extraction and was removed).
}

TEST_F(UnitTest, Events_Projector_DuplicateJob_DoesNotResetFeedExportedFlag)
{
    const auto paths = makeTempEventsPaths("project_duplicate_job_preserves_exported");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(
        140,
        0,
        1,
        0xB7,
        0xD7,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'020'000);
    const events::ChainBlockInfo block = makeBlockInfo(
        140,
        event.raw.block_hash,
        hexBytes(0x8A, 32),
        1'700'020'000,
        1'700'020'000'100);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 141, 1'700'020'000'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'020'000'300), 1u);

    const events::FinalityHeights heights{
        .head = 200,
        .safe = 140,
        .finalized = 140
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, 1'700'020'000'400, 2048));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'020'000'500), 1u);

    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 36500, 1'700'020'010'000));

    {
        // Simulate re-projection by resetting feed_cursor to 0, then re-projecting.
        // This tests that re-processing an already-exported feed row preserves exported=1.
        SqliteWritable fdb(paths.feed_db);
        fdb.exec("UPDATE feed_cursor SET last_change_seq=0 WHERE singleton=1;");
    }

    EXPECT_GE(awaitProjectBatch(store_io_context, store, feed_obj, 32, 1'700'020'020'000), 1u);

    SqliteReadonly fdb(paths.feed_db);
    EXPECT_EQ(fdb.scalarInt64("SELECT exported FROM feed_items_hot LIMIT 1;"), 1);
    EXPECT_EQ(fdb.scalarInt64("SELECT COUNT(1) FROM global_outbox;"), 2);
}

TEST_F(UnitTest, Events_Projector_StreamSeq_RemainsMonotonicAcrossPruneAndRestart)
{
    const auto paths = makeTempEventsPaths("project_stream_seq_monotonic_prune_restart");

    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db);
        feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 10);
        const events::DecodedEvent first =
            makeDecodedEvent(90, 0, 1, 0xC8, 0xE8, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'900);
        const events::ChainBlockInfo first_block =
            makeBlockInfo(90, first.raw.block_hash, hexBytes(0x70, 32), 1'700'000'900, 1'700'000'900'100);
        ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {first}, {first_block}, 91, 1'700'000'900'200));
        EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'900'300), 1u);

        // Trigger retention prune so the outbox can become empty.
        EXPECT_EQ(awaitProjectBatch(store_io_context, store, feed_obj, 16, 1'700'000'901'000), 0u);
        events_sql::expectRowCount(paths.feed_db, "global_outbox", 0);
    }

    {
        asio::io_context restarted_io_context;
        events::SQLiteHotStore restarted(paths.hot_db);
        feed::Feed feed_obj(restarted_io_context, paths.feed_db, paths.feed_archive_root, 10);
        const events::DecodedEvent second = makeDecodedEvent(
            91,
            0,
            1,
            0xC9,
            0xE9,
            events::EventType::TRANSFORMATION_ADDED,
            events::EventState::OBSERVED,
            1'700'000'901);
        const events::ChainBlockInfo second_block =
            makeBlockInfo(91, second.raw.block_hash, hexBytes(0x71, 32), 1'700'000'901, 1'700'000'901'100);
        ASSERT_TRUE(awaitIngestBatch(
            restarted_io_context,
            restarted,
            CHAIN_ID,
            {second},
            {second_block},
            92,
            1'700'000'901'200));
        EXPECT_EQ(projectAll(restarted_io_context, restarted, feed_obj, 1'700'000'901'300), 1u);
    }

    SqliteReadonly fdb(paths.feed_db);
    EXPECT_EQ(fdb.scalarInt64("SELECT stream_seq FROM global_outbox LIMIT 1;"), 2);
}

TEST_F(UnitTest, Events_Projector_ReplayedLocalEmission_DeduplicatesByTypeNameOwner)
{
    const auto paths = makeTempEventsPaths("project_replayed_local_dedup");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    events::DecodedEvent first = makeDecodedEvent(
        300,
        0,
        1,
        0xD1,
        0xE1,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::FINALIZED,
        1'700'031'000);
    first.name = "shared_entity";
    first.owner = hexAddress(0x66);

    const events::ChainBlockInfo first_block = makeBlockInfo(
        300,
        first.raw.block_hash,
        hexBytes(0x20, 32),
        1'700'031'000,
        1'700'031'000'100);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {first},
        {first_block},
        301,
        1'700'031'000'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'031'000'300), 1u);

    events::DecodedEvent replay = makeDecodedEvent(
        301,
        2,
        7,
        0xD2,
        0xE2,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::FINALIZED,
        1'700'031'100);
    replay.name = first.name;
    replay.owner = first.owner;

    const events::ChainBlockInfo replay_block = makeBlockInfo(
        301,
        replay.raw.block_hash,
        hexBytes(0x21, 32),
        1'700'031'100,
        1'700'031'100'100);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {replay},
        {replay_block},
        302,
        1'700'031'100'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'031'100'300), 1u);

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 1);

    SqliteReadonly fdb(paths.feed_db);
    EXPECT_EQ(fdb.scalarText("SELECT tx_hash FROM feed_items_hot LIMIT 1;"), replay.raw.tx_hash);
    EXPECT_EQ(fdb.scalarInt64("SELECT log_index FROM feed_items_hot LIMIT 1;"), replay.raw.log_index);
    EXPECT_EQ(fdb.scalarText("SELECT feed_id FROM feed_items_hot LIMIT 1;").rfind("local:1:", 0), 0u);
}

TEST_F(UnitTest, Events_ReadChangesSince_ReturnsOrderedWithRawBytes)
{
    const auto paths = makeTempEventsPaths("read_changes");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db);
    auto ev = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'500);
    auto blk = makeBlockInfo(60, ev.raw.block_hash, hexBytes(0x91,32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {ev}, {blk}, 61, 1'700'000'501'100));
    const auto all = store.readChangesSince(0, 10);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_GT(all[0].change_seq, 0);
    EXPECT_FALSE(all[0].data_hex.empty());
    EXPECT_TRUE(store.readChangesSince(all[0].change_seq, 10).empty());
}

TEST_F(UnitTest, Events_Projector_OrphanJobs_AreCleanedBeforeProjection)
{
    const auto paths = makeTempEventsPaths("project_orphan_cleanup");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    // With no ingested events, projectBatch must return 0.
    EXPECT_EQ(awaitProjectBatch(store_io_context, store, feed_obj, 64, 1'700'000'000'500), 0u);
}

TEST_F(UnitTest, Events_NoSharedProjectionQueue)
{
    const auto paths = makeTempEventsPaths("no_shared_queue");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db);
    EXPECT_EQ(events_sql::scalarInt(paths.hot_db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('projection_jobs','projector_progress')"), 0);
}

TEST_F(UnitTest, Events_Projector_JobsArePartitionedByProjectorId)
{
    const auto paths = makeTempEventsPaths("jobs_partitioned");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db);
    const auto event = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'500);
    const auto block = makeBlockInfo(60, event.raw.block_hash, hexBytes(0x91,32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {event}, {block}, 61, 1'700'000'501'100));
    // With the changelog-based model, ingest creates a change_seq entry in normalized_events_hot.
    const auto changes = store.readChangesSince(0, 10);
    EXPECT_EQ(changes.size(), 1u);
}

TEST_F(UnitTest, Events_RegistryPartition_EnqueuedOnFinalizedAndClaimable)
{
    const auto paths = makeTempEventsPaths("registry_partition");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db);

    // Observed: no finalized rows yet.
    auto ev = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'500);
    auto blk = makeBlockInfo(60, ev.raw.block_hash, hexBytes(0x91,32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {ev}, {blk}, 61, 1'700'000'501'100));

    // Drive to finalized -> the changelog now has the finality transition.
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, /*head*/60, /*safe*/60, /*finalized*/60, 1'700'000'502'000));
    const auto changes = store.readChangesSince(0, 10);
    // readChangesSince returns the *current* state of rows with change_seq > threshold.
    // observed->safe->finalized all happen in one applyFinality call, so the single
    // normalized row ends with its final change_seq (finalized state).  Exactly 1 change.
    EXPECT_GE(changes.size(), 1u);
    // The finalized row should appear in the changelog.
    const auto finalized_it = std::find_if(changes.begin(), changes.end(),
        [](const events::ChangeRecord& r) { return r.state == "finalized"; });
    EXPECT_NE(finalized_it, changes.end());
}

TEST_F(UnitTest, Events_FeedProjector_ConsumesChangelogAndAdvancesCursor)
{
    const auto paths = makeTempEventsPaths("feed_changelog");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(io, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);
    auto strand = asio::make_strand(io);
    feed::FeedProjector proj(store, feed_obj, strand);
    auto ev = makeDecodedEvent(60,0,1,0xB0,0xD0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'500);
    auto blk = makeBlockInfo(60, ev.raw.block_hash, hexBytes(0x91,32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {ev}, {blk}, 61, 1'700'000'501'100));
    const std::size_t n = runAwaitable(io, proj.projectBatch(256, 1'700'000'502'000));
    EXPECT_EQ(n, 1u);
    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
    EXPECT_GT(proj.cursor(), 0);
}

TEST_F(UnitTest, Events_FeedProjector_OutboxMaintenance_PrunesOldRowsAndAdvancesFloor)
{
    // Verify that FeedProjector::projectBatch runs outbox retention + floor alignment
    // the same way the old SQLiteHotStore::projectBatch did.
    const auto paths = makeTempEventsPaths("feed_outbox_maintenance");
    asio::io_context io;
    // retention = 10 ms — any row older than 10 ms is pruned
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(io, paths.feed_db, paths.feed_archive_root, 10);
    auto strand = asio::make_strand(io);
    feed::FeedProjector proj(store, feed_obj, strand);

    // Ingest and project one event at t=1'700'000'900'300; the outbox row gets
    // created_at_ms = 1'700'000'900'300.
    auto ev = makeDecodedEvent(90, 0, 1, 0xC8, 0xE8,
        events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'900);
    auto blk = makeBlockInfo(90, ev.raw.block_hash, hexBytes(0x70,32),
        1'700'000'900, 1'700'000'900'100);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {ev}, {blk}, 91, 1'700'000'900'200));

    const std::size_t projected = runAwaitable(io, proj.projectBatch(256, 1'700'000'900'300));
    EXPECT_EQ(projected, 1u);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 1);

    // Run another FeedProjector::projectBatch pass at t=1'700'000'901'000 (700 ms later).
    // cutoff = 1'700'000'901'000 - 10 = 1'700'000'900'990 > 1'700'000'900'300 so the row
    // is beyond the retention window and must be pruned.
    const std::size_t pruned_pass = runAwaitable(io, proj.projectBatch(256, 1'700'000'901'000));
    EXPECT_EQ(pruned_pass, 0u);

    // The outbox row must have been deleted.
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 0);

    // replay_floor_seq must have advanced to max_pruned_seq+1 = 2 (stream_seq was 1).
    SqliteReadonly fdb(paths.feed_db);
    EXPECT_GE(fdb.scalarInt64("SELECT replay_floor_seq FROM outbox_stream_state WHERE singleton=1;"), 2);
}
