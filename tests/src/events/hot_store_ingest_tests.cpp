#include "unit-tests.hpp"

#include "events_sql_assertions.hpp"
#include "events_test_harness.hpp"
#include "feed.hpp"
#include "feed_projector.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

TEST_F(UnitTest, Events_HotStore_IngestSingleEvent_WritesDurableRows)
{
    const auto paths = makeTempEventsPaths("ingest_single");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    events::DecodedEvent event = makeDecodedEvent(11, 1, 2, 0xA0, 0xB0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'000);
    const events::ChainBlockInfo block_info = makeBlockInfo(11, event.raw.block_hash, hexBytes(0x99, 32), 1'700'000'000, 1'700'000'001'000);

    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block_info}, 12, 1'700'000'001'100));

    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 1);
    events_sql::expectRowCount(paths.hot_db, "normalized_events_hot", 1);
    events_sql::expectRowCount(paths.hot_db, "reorg_window", 1);
    events_sql::expectRowCount(paths.hot_db, "source_resume", 1);

    const auto changes = store.readChangesSince(0, 10);
    EXPECT_EQ(changes.size(), 1u);

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT next_cursor FROM source_resume WHERE source_id='local';"), 12);
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot LIMIT 1;"), "observed");
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot LIMIT 1;"), "observed");
}

TEST_F(UnitTest, Events_HotStore_UndecodableRawLog_IsDurableAndCursorAdvances)
{
    const auto paths = makeTempEventsPaths("ingest_decode_failure_durable");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    events::RawChainLog raw = makeRawLog(
        21,
        0,
        2,
        hexBytes(0xA1, 32),
        hexBytes(0xB2, 32),
        "0x12345678",
        "0x01",
        false,
        1'700'000'100,
        1'700'000'101'000);
    raw.topics[1] = hexBytes(0x22, 32);

    const events::ChainBlockInfo block_info = makeBlockInfo(21, raw.block_hash, hexBytes(0x98, 32), 1'700'000'100, 1'700'000'101'000);

    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {raw}, {}, {block_info}, 22, 1'700'000'101'500));

    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 1);
    events_sql::expectRowCount(paths.hot_db, "normalized_events_hot", 0);
    events_sql::expectRowCount(paths.hot_db, "decode_failures_hot", 1);

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT next_cursor FROM source_resume WHERE source_id='local';"), 22);
    EXPECT_EQ(db.scalarInt64("SELECT attempts FROM decode_failures_hot WHERE chain_id=1;"), 1);

    events::DecodedEvent recovered = makeDecodedEvent(
        21,
        0,
        2,
        0xA1,
        0xB2,
        events::EventType::TRANSFORMATION_ADDED,
        events::EventState::OBSERVED,
        1'700'000'100,
        1'700'000'102'000);
    recovered.raw.block_hash = raw.block_hash;
    recovered.raw.tx_hash = raw.tx_hash;
    recovered.raw.log_index = raw.log_index;
    recovered.raw.block_number = raw.block_number;
    recovered.raw.tx_index = raw.tx_index;
    recovered.raw.address = raw.address;
    recovered.raw.data_hex = raw.data_hex;
    recovered.raw.topics = raw.topics;
    recovered.raw.block_time = raw.block_time;
    recovered.raw.parent_hash = raw.parent_hash;
    recovered.raw.removed = false;

    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {raw}, {recovered}, {block_info}, 23, 1'700'000'102'100));
    events_sql::expectRowCount(paths.hot_db, "decode_failures_hot", 0);
    events_sql::expectRowCount(paths.hot_db, "normalized_events_hot", 1);
}

TEST_F(UnitTest, Events_HotStore_DuplicateIngest_IsIdempotent)
{
    const auto paths = makeTempEventsPaths("ingest_idempotent");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent event = makeDecodedEvent(31, 3, 4, 0xA2, 0xB3, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'200);
    const events::ChainBlockInfo block_info = makeBlockInfo(31, event.raw.block_hash, hexBytes(0x97, 32), 1'700'000'200, 1'700'000'201'000);

    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block_info}, 32, 1'700'000'201'100));
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block_info}, 32, 1'700'000'201'200));

    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 1);
    events_sql::expectRowCount(paths.hot_db, "normalized_events_hot", 1);
}

// One cursor row per source: the persisted cursor_kind decides how next_cursor is
// read back, and a lookup under the other kind must miss.
TEST_F(UnitTest, Events_HotStore_SourceCursor_PersistsAcrossRestart)
{
    const auto paths = makeTempEventsPaths("ingest_resume_source_cursor");

    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
        const events::DecodedEvent event = makeDecodedEvent(41, 0, 1, 0xA4, 0xB4, events::EventType::CONDITION_ADDED, events::EventState::OBSERVED, 1'700'000'300);
        const events::ChainBlockInfo block_info = makeBlockInfo(41, event.raw.block_hash, hexBytes(0x96, 32), 1'700'000'300, 1'700'000'301'000);
        ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block_info}, 42, 1'700'000'301'200));
    }

    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
        const auto next_block = awaitLoadNextCursor(store_io_context, store, events::CursorKind::NEXT_BLOCK);
        ASSERT_TRUE(next_block.has_value());
        EXPECT_EQ(*next_block, 42u);

        EXPECT_FALSE(awaitLoadNextCursor(store_io_context, store, events::CursorKind::LOCAL_SEQUENCE).has_value());
        EXPECT_FALSE(awaitLoadNextCursor(store_io_context, store, events::CursorKind::NEXT_BLOCK, "other").has_value());
    }
}

TEST_F(UnitTest, Events_HotStore_LocalSequenceCursor_CommitsWithDurableIngest)
{
    const auto paths = makeTempEventsPaths("ingest_local_seq_txn");

    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
        const events::DecodedEvent event = makeDecodedEvent(
            42,
            0,
            1,
            0xA5,
            0xB5,
            events::EventType::CONNECTOR_ADDED,
            events::EventState::OBSERVED,
            1'700'000'320);
        const events::ChainBlockInfo block_info =
            makeBlockInfo(42, event.raw.block_hash, hexBytes(0x95, 32), 1'700'000'320, 1'700'000'321'000);
        ASSERT_TRUE(awaitIngestBatch(
            store_io_context,
            store,
            CHAIN_ID,
            {event.raw},
            {event},
            {block_info},
            43,
            1'700'000'321'200,
            777));
    }

    {
        asio::io_context restarted_io_context;
        events::SQLiteHotStore restarted(paths.hot_db, CHAIN_ID);
        const auto next_seq = awaitLoadNextCursor(restarted_io_context, restarted, events::CursorKind::LOCAL_SEQUENCE);
        ASSERT_TRUE(next_seq.has_value());
        EXPECT_EQ(*next_seq, 777u);

        EXPECT_FALSE(awaitLoadNextCursor(restarted_io_context, restarted, events::CursorKind::NEXT_BLOCK).has_value());
    }
}

TEST_F(UnitTest, Events_HotStore_ReorgReplacement_MarksOldRowsRemovedAndQueuesJobs)
{
    const auto paths = makeTempEventsPaths("ingest_reorg_replace");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);

    events::DecodedEvent old_event = makeDecodedEvent(50, 0, 1, 0xAA, 0xC1, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'400);
    const events::ChainBlockInfo old_block = makeBlockInfo(50, old_event.raw.block_hash, hexBytes(0x95, 32), 1'700'000'400, 1'700'000'401'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {old_event}, {old_block}, 51, 1'700'000'401'100));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'401'200), 1u);

    events::DecodedEvent replacement = makeDecodedEvent(50, 1, 2, 0xAB, 0xC2, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'401);
    const events::ChainBlockInfo replacement_block = makeBlockInfo(50, replacement.raw.block_hash, hexBytes(0x94, 32), 1'700'000'401, 1'700'000'402'000);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {replacement},
        {replacement_block},
        51,
        1'700'000'402'100));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(
        db.scalarText(std::format(
            "SELECT state FROM normalized_events_hot WHERE chain_id=1 AND block_hash='{}' AND log_index={};",
            old_event.raw.block_hash,
            old_event.raw.log_index)),
        "removed");
    EXPECT_EQ(
        db.scalarInt64(std::format(
            "SELECT removed FROM raw_events_hot WHERE chain_id=1 AND block_hash='{}' AND log_index={};",
            old_event.raw.block_hash,
            old_event.raw.log_index)),
        1);
    EXPECT_EQ(
        db.scalarText("SELECT block_hash FROM reorg_window WHERE chain_id=1 AND block_number=50;"),
        replacement.raw.block_hash);
}

TEST_F(UnitTest, Events_HotStore_RemovedRawWithoutDecodedEvent_StillInvalidatesNormalizedFeedState)
{
    const auto paths = makeTempEventsPaths("ingest_removed_raw_without_decoded");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);

    const events::DecodedEvent inserted = makeDecodedEvent(
        75,
        0,
        1,
        0xBC,
        0xCD,
        events::EventType::TRANSFORMATION_ADDED,
        events::EventState::OBSERVED,
        1'700'003'100);
    const events::ChainBlockInfo block_info = makeBlockInfo(
        75,
        inserted.raw.block_hash,
        hexBytes(0x66, 32),
        1'700'003'100,
        1'700'003'100'100);

    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {inserted}, {block_info}, 76, 1'700'003'100'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'003'100'300), 1u);

    events::RawChainLog removed_raw = inserted.raw;
    removed_raw.removed = true;
    removed_raw.seen_at_ms = 1'700'003'100'400;

    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {removed_raw},
        {},
        {block_info},
        76,
        1'700'003'100'500));

    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'003'100'600), 1u);

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot LIMIT 1;"), "removed");
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot LIMIT 1;"), "removed");
    SqliteReadonly feed_db(paths.feed_db);
    EXPECT_EQ(feed_db.scalarText("SELECT status FROM feed_items_hot LIMIT 1;"), "removed");
    EXPECT_EQ(feed_db.scalarInt64("SELECT visible FROM feed_items_hot LIMIT 1;"), 0);
}

TEST_F(UnitTest, Events_HotStore_RemovedRawWithDecodedObserved_StillPinsRawStateToRemoved)
{
    const auto paths = makeTempEventsPaths("ingest_removed_raw_with_decoded_observed");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);

    const events::DecodedEvent inserted = makeDecodedEvent(
        88,
        0,
        1,
        0xAD,
        0xBD,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'004'000);
    const events::ChainBlockInfo block_info = makeBlockInfo(
        88,
        inserted.raw.block_hash,
        hexBytes(0x65, 32),
        1'700'004'000,
        1'700'004'000'100);

    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {inserted}, {block_info}, 89, 1'700'004'000'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'004'000'300), 1u);

    events::RawChainLog removed_raw = inserted.raw;
    removed_raw.removed = true;
    removed_raw.seen_at_ms = 1'700'004'000'400;

    events::DecodedEvent decoded_observed = inserted;
    decoded_observed.raw = removed_raw;
    decoded_observed.state = events::EventState::OBSERVED;

    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {removed_raw},
        {decoded_observed},
        {block_info},
        89,
        1'700'004'000'500));

    const events::FinalityHeights heights{
        .head = 200,
        .safe = 88,
        .finalized = 88
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, 1'700'004'000'600, 2048));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot LIMIT 1;"), "removed");
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot LIMIT 1;"), "removed");
}

TEST_F(UnitTest, Events_ChangeSeq_BumpsOnInsertAndEachTransition)
{
    const auto paths = makeTempEventsPaths("change_seq");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    auto ev = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'500);
    auto blk = makeBlockInfo(60, ev.raw.block_hash, hexBytes(0x91,32), 1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {ev}, {blk}, 61, 1'700'000'501'100));
    const std::int64_t s_observed = events_sql::scalarInt(paths.hot_db,
        "SELECT change_seq FROM normalized_events_hot LIMIT 1");
    ASSERT_GT(s_observed, 0);
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 60, 60, 60, 1'700'000'502'000));
    const std::int64_t s_final = events_sql::scalarInt(paths.hot_db,
        "SELECT change_seq FROM normalized_events_hot LIMIT 1");
    EXPECT_GT(s_final, s_observed);  // finalized transition bumped change_seq
}

TEST_F(UnitTest, Events_ChangeSeq_DistinctPerRowOnBatchFinality)
{
    // Prove that when multiple rows are finalized in one applyFinality call,
    // each row receives a DISTINCT change_seq — no two rows share the same value.
    const auto paths = makeTempEventsPaths("change_seq_distinct");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    // Two events in the SAME block, different log_index values.
    auto ev0 = makeDecodedEvent(70, 0, 0, 0xC0, 0xE0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'001'000);
    auto ev1 = makeDecodedEvent(70, 0, 1, 0xC0, 0xE0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'001'000);
    // Share block_hash (same block_hash_byte 0xC0), different log_index.
    ev1.raw.block_hash = ev0.raw.block_hash;
    ev1.raw.tx_hash    = ev0.raw.tx_hash;

    const auto blk = makeBlockInfo(70, ev0.raw.block_hash, hexBytes(0x90, 32), 1'700'001'000, 1'700'001'001'000);

    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {ev0, ev1}, {blk}, 71, 1'700'001'001'100));

    // Finalize both events in ONE applyFinality call (head=safe=finalized=70).
    ASSERT_TRUE(awaitApplyFinality(io, store, CHAIN_ID, 70, 70, 70, 1'700'001'002'000));

    // Every row must have a distinct change_seq: COUNT(DISTINCT change_seq) == COUNT(*).
    const std::int64_t total = events_sql::scalarInt(paths.hot_db,
        "SELECT COUNT(*) FROM normalized_events_hot");
    const std::int64_t distinct = events_sql::scalarInt(paths.hot_db,
        "SELECT COUNT(DISTINCT change_seq) FROM normalized_events_hot");

    ASSERT_EQ(total, 2);
    EXPECT_EQ(distinct, total)
        << "Two rows finalized in one applyFinality call share a change_seq "
           "— projector cursor will silently skip events";
}
