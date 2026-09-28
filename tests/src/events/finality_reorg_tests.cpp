#include "unit-tests.hpp"

#include "events_sql_assertions.hpp"
#include "events_test_harness.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

TEST_F(UnitTest, Events_Finality_ObservedToFinalized_TransitionsDurably)
{
    const auto paths = makeTempEventsPaths("finality_transition");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent event = makeDecodedEvent(70, 1, 1, 0xC0, 0xE0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'600);
    const events::ChainBlockInfo block = makeBlockInfo(70, event.raw.block_hash, hexBytes(0x8A, 32), 1'700'000'600, 1'700'000'601'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 71, 1'700'000'601'100));

    const events::FinalityHeights heights{
        .head = 90,
        .safe = 70,
        .finalized = 70
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, 1'700'000'602'000, 2048));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot LIMIT 1;"), "finalized");
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot LIMIT 1;"), "finalized");
    EXPECT_EQ(db.scalarInt64("SELECT head_block FROM finality_state WHERE chain_id=1;"), 90);
    EXPECT_EQ(db.scalarInt64("SELECT safe_block FROM finality_state WHERE chain_id=1;"), 70);
    EXPECT_EQ(db.scalarInt64("SELECT finalized_block FROM finality_state WHERE chain_id=1;"), 70);
}

TEST_F(UnitTest, Events_Finality_HeightBelowEvent_DoesNotAdvanceState)
{
    const auto paths = makeTempEventsPaths("finality_no_advance");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent event = makeDecodedEvent(71, 0, 2, 0xC1, 0xE1, events::EventType::CONDITION_ADDED, events::EventState::OBSERVED, 1'700'000'610);
    const events::ChainBlockInfo block = makeBlockInfo(71, event.raw.block_hash, hexBytes(0x89, 32), 1'700'000'610, 1'700'000'611'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 72, 1'700'000'611'100));

    const events::FinalityHeights heights{
        .head = 80,
        .safe = 70,
        .finalized = 70
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, 1'700'000'612'000, 2048));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot LIMIT 1;"), "observed");
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot LIMIT 1;"), "observed");
}

TEST_F(UnitTest, Events_Finality_PrunesReorgWindowByHeadMinusWindow)
{
    const auto paths = makeTempEventsPaths("finality_reorg_prune");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const auto block_80 = makeBlockInfo(80, hexBytes(0x80, 32), hexBytes(0x79, 32), 1'700'000'620, 1'700'000'620'000);
    const auto block_95 = makeBlockInfo(95, hexBytes(0x95, 32), hexBytes(0x94, 32), 1'700'000'621, 1'700'000'621'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {}, {block_80, block_95}, 96, 1'700'000'621'500));

    const events::FinalityHeights heights{
        .head = 100,
        .safe = 99,
        .finalized = 98
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, 1'700'000'622'000, 10));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM reorg_window;"), 1);
    EXPECT_EQ(db.scalarInt64("SELECT block_number FROM reorg_window LIMIT 1;"), 95);
}

TEST_F(UnitTest, Events_Reorg_RemovedState_PropagatesToFeedAndOutbox)
{
    const auto paths = makeTempEventsPaths("reorg_removed_feed");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);

    events::DecodedEvent inserted = makeDecodedEvent(72, 1, 1, 0xC2, 0xE2, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'630);
    const events::ChainBlockInfo block = makeBlockInfo(72, inserted.raw.block_hash, hexBytes(0x88, 32), 1'700'000'630, 1'700'000'631'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {inserted}, {block}, 73, 1'700'000'631'100));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'631'200), 1u);

    events::DecodedEvent removed = inserted;
    removed.state = events::EventState::REMOVED;
    removed.raw.removed = true;
    removed.raw.seen_at_ms = 1'700'000'632'000;
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {removed}, {block}, 73, 1'700'000'632'100));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'632'200), 1u);

    SqliteReadonly feed_db(paths.feed_db);
    EXPECT_EQ(feed_db.scalarText("SELECT status FROM feed_items_hot LIMIT 1;"), "removed");
    EXPECT_EQ(feed_db.scalarInt64("SELECT visible FROM feed_items_hot LIMIT 1;"), 0);
    EXPECT_EQ(feed_db.scalarText("SELECT op FROM global_outbox WHERE stream_seq=2;"), "remove");
    EXPECT_EQ(feed_db.scalarText("SELECT status FROM global_outbox WHERE stream_seq=2;"), "removed");
}

TEST_F(UnitTest, Events_IngestLookback_UnchangedCanonicalRows_DoNotRegressOrEmitDuplicateOutbox)
{
    const auto paths = makeTempEventsPaths("finality_lookback_no_churn");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);

    const events::DecodedEvent event = makeDecodedEvent(
        120,
        0,
        1,
        0xD0,
        0xF0,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'000'720);
    const events::ChainBlockInfo block = makeBlockInfo(120, event.raw.block_hash, hexBytes(0x81, 32), 1'700'000'720, 1'700'000'721'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 121, 1'700'000'721'100));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'721'200), 1u);

    const events::FinalityHeights heights{
        .head = 200,
        .safe = 120,
        .finalized = 120
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, 1'700'000'721'300, 2048));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'721'400), 1u);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 2);

    events::DecodedEvent lookback_refetch = event;
    lookback_refetch.state = events::EventState::OBSERVED;
    lookback_refetch.raw.seen_at_ms = 1'700'000'721'500;
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {lookback_refetch},
        {block},
        121,
        1'700'000'721'600));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'721'700), 0u);

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot LIMIT 1;"), "finalized");
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot LIMIT 1;"), "finalized");
    SqliteReadonly feed_db(paths.feed_db);
    EXPECT_EQ(feed_db.scalarInt64("SELECT COUNT(1) FROM global_outbox;"), 2);
}

TEST_F(UnitTest, Events_Finality_Invariants_AreClampedBeforePersistence)
{
    const auto paths = makeTempEventsPaths("finality_clamp_invariants");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::FinalityHeights invalid{
        .head = 100,
        .safe = 120,
        .finalized = 150
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, invalid, 1'700'000'800'000, 2048));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT head_block FROM finality_state WHERE chain_id=1;"), 100);
    EXPECT_EQ(db.scalarInt64("SELECT safe_block FROM finality_state WHERE chain_id=1;"), 100);
    EXPECT_EQ(db.scalarInt64("SELECT finalized_block FROM finality_state WHERE chain_id=1;"), 100);
}

TEST_F(UnitTest, Events_Finality_StatePersistence_IsMonotonicAcrossProviderRegression)
{
    const auto paths = makeTempEventsPaths("finality_monotonic_persistence");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::FinalityHeights first{
        .head = 500,
        .safe = 490,
        .finalized = 480
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, first, 1'700'000'900'000, 2048));

    const events::FinalityHeights regressed{
        .head = 450,
        .safe = 430,
        .finalized = 420
    };
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, regressed, 1'700'000'900'100, 2048));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT head_block FROM finality_state WHERE chain_id=1;"), 500);
    EXPECT_EQ(db.scalarInt64("SELECT safe_block FROM finality_state WHERE chain_id=1;"), 490);
    EXPECT_EQ(db.scalarInt64("SELECT finalized_block FROM finality_state WHERE chain_id=1;"), 480);
}

