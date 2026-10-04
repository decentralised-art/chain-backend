#include <gtest/gtest.h>
#include "unit-tests.hpp"
#include "events_test_harness.hpp"
#include "sqlite_hot_store.hpp"
#include "feed.hpp"
#include "feed_projector.hpp"
#include "feed_runtime.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

TEST_F(UnitTest, Feed_EventsDb_HasNoFeedTables)
{
    // Regression guard: feed tables must live in the feed DB, not the events hot DB.
    const auto paths = makeTempEventsPaths("feed_decoupling");
    asio::io_context io;
    events::SQLiteHotStore store(paths.hot_db);

    SqliteReadonly db(paths.hot_db);
    const auto no_table = [&](const std::string & table_name)
    {
        EXPECT_EQ(db.scalarInt64(std::format(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='{}';",
            table_name)), 0)
            << table_name << " should not exist in the events hot DB";
    };
    no_table("feed_items_hot");
    no_table("global_outbox");
    no_table("outbox_stream_state");
    no_table("feed_cursor");
}

TEST_F(UnitTest, Feed_Projector_ConsumesEventsChangelogIntoFeedDb)
{
    const auto epaths = makeTempEventsPaths("feed_proj_events");
    const auto fpaths = makeTempEventsPaths("feed_proj_feed");
    asio::io_context io;

    events::SQLiteHotStore store(epaths.hot_db);
    dcn::feed::Feed feed_obj(io, fpaths.feed_db, fpaths.feed_archive_root, 7LL*24*60*60*1000);
    auto strand = asio::make_strand(io);
    dcn::feed::FeedProjector proj(store, feed_obj, strand);

    auto evd = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0,
                                events::EventType::CONNECTOR_ADDED,
                                events::EventState::OBSERVED,
                                1'700'000'500);
    auto blk = makeBlockInfo(60, evd.raw.block_hash, hexBytes(0x91, 32),
                             1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {evd}, {blk}, 61, 1'700'000'501'100));

    const std::size_t n = runAwaitable(io, proj.projectBatch(256, 1'700'000'502'000));
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(feed_obj.getFeedPage(dcn::feed::FeedQuery{.limit = 10}).items.size(), 1u);
    EXPECT_GT(proj.cursor(), 0);
}

TEST_F(UnitTest, Feed_Projector_DeadLetteredRow_IsRecoveredBySweep)
{
    const auto epaths = makeTempEventsPaths("feed_proj_dl_events");
    const auto fpaths = makeTempEventsPaths("feed_proj_dl_feed");
    asio::io_context io;

    events::SQLiteHotStore store(epaths.hot_db);
    dcn::feed::Feed feed_obj(io, fpaths.feed_db, fpaths.feed_archive_root, 7LL*24*60*60*1000);
    auto strand = asio::make_strand(io);

    auto evd = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0,
                                events::EventType::CONNECTOR_ADDED,
                                events::EventState::OBSERVED,
                                1'700'000'500);
    auto blk = makeBlockInfo(60, evd.raw.block_hash, hexBytes(0x91, 32),
                             1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {evd}, {blk}, 61, 1'700'000'501'100));

    // Quarantined history: the row is dead-lettered and the feed cursor is already
    // past it, exactly the state an exhausted retry budget leaves behind.
    const auto changes = store.readChangesSince(0, 10);
    ASSERT_EQ(changes.size(), 1u);
    ASSERT_TRUE(store.markDeadLetter(
        events::FEED_DEAD_LETTER_BIT, changes.front().source_id,
        changes.front().block_hash, changes.front().log_index));
    runAwaitable(io, feed_obj.setFeedCursor(changes.front().change_seq));

    dcn::feed::FeedProjector proj(store, feed_obj, strand);
    EXPECT_EQ(proj.cursor(), changes.front().change_seq);
    EXPECT_EQ(feed_obj.getFeedPage(dcn::feed::FeedQuery{.limit = 10}).items.size(), 0u);

    // Idle pass → the sweep applies the quarantined row into the feed and clears
    // its dead-letter bit, so nothing is lost despite the cursor being past it.
    const std::size_t n = runAwaitable(io, proj.projectBatch(256, 1'700'000'502'000));
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(feed_obj.getFeedPage(dcn::feed::FeedQuery{.limit = 10}).items.size(), 1u);
    EXPECT_TRUE(store.readDeadLetters(events::FEED_DEAD_LETTER_BIT, 10).empty());
}

TEST_F(UnitTest, Feed_Projector_DeadLetterSweepInterval_IsConfigurable)
{
    const auto epaths = makeTempEventsPaths("feed_proj_sweep_interval_events");
    const auto fpaths = makeTempEventsPaths("feed_proj_sweep_interval_feed");
    asio::io_context io;

    events::SQLiteHotStore store(epaths.hot_db);
    dcn::feed::Feed feed_obj(io, fpaths.feed_db, fpaths.feed_archive_root, 7LL*24*60*60*1000);
    auto strand = asio::make_strand(io);

    auto evd = makeDecodedEvent(60, 0, 1, 0xB0, 0xD0,
                                events::EventType::CONNECTOR_ADDED,
                                events::EventState::OBSERVED,
                                1'700'000'500);
    auto blk = makeBlockInfo(60, evd.raw.block_hash, hexBytes(0x91, 32),
                             1'700'000'500, 1'700'000'501'000);
    ASSERT_TRUE(awaitIngestBatch(io, store, CHAIN_ID, {evd}, {blk}, 61, 1'700'000'501'100));

    const auto changes = store.readChangesSince(0, 10);
    ASSERT_EQ(changes.size(), 1u);
    runAwaitable(io, feed_obj.setFeedCursor(changes.front().change_seq));

    const dcn::events::ProjectorRetryConfig retry_cfg{.sweep_interval_ms = 10'000};
    dcn::feed::FeedProjector proj(store, feed_obj, strand, retry_cfg);

    // First idle pass at T: nothing is dead-lettered yet, but the sweep runs and
    // anchors its timestamp at T.
    const std::int64_t t0 = 1'700'000'600'000;
    EXPECT_EQ(runAwaitable(io, proj.projectBatch(256, t0)), 0u);

    // Quarantine the (recoverable) row after the anchor pass.
    ASSERT_TRUE(store.markDeadLetter(
        events::FEED_DEAD_LETTER_BIT, changes.front().source_id,
        changes.front().block_hash, changes.front().log_index));

    // Inside the interval: the sweep is throttled, so the row is not applied.
    EXPECT_EQ(runAwaitable(io, proj.projectBatch(256, t0 + retry_cfg.sweep_interval_ms - 1)), 0u);
    EXPECT_EQ(feed_obj.getFeedPage(dcn::feed::FeedQuery{.limit = 10}).items.size(), 0u);

    // At the interval boundary: the sweep runs, applies the row, clears the bit.
    EXPECT_EQ(runAwaitable(io, proj.projectBatch(256, t0 + retry_cfg.sweep_interval_ms)), 1u);
    EXPECT_EQ(feed_obj.getFeedPage(dcn::feed::FeedQuery{.limit = 10}).items.size(), 1u);
    EXPECT_TRUE(store.readDeadLetters(events::FEED_DEAD_LETTER_BIT, 10).empty());
}

TEST_F(UnitTest, Feed_Runtime_StartStop_ArchiveLoopRuns)
{
    const auto fpaths = makeTempEventsPaths("feed_runtime");
    asio::io_context io;
    dcn::feed::FeedRuntime runtime(io, dcn::feed::FeedRuntimeConfig{
        .feed_db_path = fpaths.feed_db,
        .archive_root = fpaths.feed_archive_root,
        .archive_interval_ms = 10,
        .wal_checkpoint_interval_ms = 10});
    runtime.start();
    // drive the io_context so the loops tick, then stop cleanly
    runAwaitable(io, runtime.stop());
    SUCCEED();  // clean start+stop with loops spawned is the assertion
}
