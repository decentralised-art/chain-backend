#include "unit-tests.hpp"

#include "events_sql_assertions.hpp"
#include "events_test_harness.hpp"

#include <atomic>
#include <thread>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

TEST_F(UnitTest, Events_Recovery_WorkerThreadWrite_IsCommitted)
{
    const auto paths = makeTempEventsPaths("recovery_worker_thread_pre_commit");

    std::atomic<bool> worker_ok{true};
    {
        asio::io_context io_context;
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

        std::thread worker([&]
        {
            const events::DecodedEvent event = makeDecodedEvent(
                110,
                0,
                1,
                0xA8,
                0x38,
                events::EventType::CONNECTOR_ADDED,
                events::EventState::OBSERVED,
                1'700'002'000);
            const events::ChainBlockInfo block =
                makeBlockInfo(110, event.raw.block_hash, hexBytes(0x50, 32), 1'700'002'000, 1'700'002'000'100);
            worker_ok.store(
                awaitIngestBatch(io_context, store, CHAIN_ID, {event}, {block}, 111, 1'700'002'000'200),
                std::memory_order_release);
        });
        worker.join();
    }
    EXPECT_TRUE(worker_ok.load(std::memory_order_acquire));

    {
        asio::io_context restarted_io_context;
        events::SQLiteHotStore restarted(paths.hot_db, CHAIN_ID);
        const auto next_from = awaitLoadNextCursor(restarted_io_context, restarted, events::CursorKind::NEXT_BLOCK);
        ASSERT_TRUE(next_from.has_value());
        EXPECT_EQ(*next_from, 111u);
    }

    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 1);
    events_sql::expectRowCount(paths.hot_db, "normalized_events_hot", 1);
}

TEST_F(UnitTest, Events_Recovery_CrashAfterIngestBeforeProjection_ProjectsAfterRestart)
{
    const auto paths = makeTempEventsPaths("recovery_post_ingest_pre_projection");

    events::DecodedEvent event;
    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
        event = makeDecodedEvent(111, 0, 1, 0xA9, 0x3A, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'002'001);
        const events::ChainBlockInfo block = makeBlockInfo(111, event.raw.block_hash, hexBytes(0x4F, 32), 1'700'002'001, 1'700'002'001'100);
        ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 112, 1'700'002'001'200));
    }

    {
        asio::io_context restarted_io_context;
        events::SQLiteHotStore restarted(paths.hot_db, CHAIN_ID);
        feed::Feed feed_obj(restarted_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);
        EXPECT_EQ(projectAll(restarted_io_context, restarted, feed_obj, 1'700'002'001'500), 1u);
    }

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
    events_sql::expectRowCount(paths.feed_db, "global_outbox", 1);
}

TEST_F(UnitTest, Events_Recovery_CrashAfterProjectionCommit_StreamReplayRemainsConsistent)
{
    const auto paths = makeTempEventsPaths("recovery_post_projection");

    {
        asio::io_context store_io_context;
        events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
        feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);
        const events::DecodedEvent first = makeDecodedEvent(112, 0, 1, 0xAA, 0x3B, events::EventType::CONDITION_ADDED, events::EventState::OBSERVED, 1'700'002'002);
        const events::DecodedEvent second = makeDecodedEvent(113, 0, 1, 0xAB, 0x3C, events::EventType::CONDITION_ADDED, events::EventState::OBSERVED, 1'700'002'003);
        ASSERT_TRUE(awaitIngestBatch(
            store_io_context,
            store,
            CHAIN_ID,
            {first, second},
            {
                makeBlockInfo(112, first.raw.block_hash, hexBytes(0x4E, 32), 1'700'002'002, 1'700'002'002'100),
                makeBlockInfo(113, second.raw.block_hash, hexBytes(0x4D, 32), 1'700'002'003, 1'700'002'003'100)
            },
            114,
            1'700'002'003'200));
        EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'002'003'300), 2u);
    }

    {
        asio::io_context restarted_io_context;
        events::SQLiteHotStore restarted(paths.hot_db, CHAIN_ID);
        feed::Feed feed_obj(restarted_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);
        const feed::StreamPage replay = feed_obj.getStreamPage(feed::StreamQuery{
            .since_seq = 1,
            .limit = 100
        });
        ASSERT_EQ(replay.deltas.size(), 1u);
        EXPECT_EQ(replay.deltas.front().stream_seq, 2);
    }
}

TEST_F(UnitTest, Events_Recovery_RpcDisconnectEquivalent_NoCorruptionOnNoopCycle)
{
    const auto paths = makeTempEventsPaths("recovery_noop_cycles");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);

    // Simulate repeated no-op polling cycles by only applying advancing finality.
    for(std::int64_t i = 0; i < 5; ++i)
    {
        const events::FinalityHeights heights{
            .head = 200 + i,
            .safe = 190 + i,
            .finalized = 180 + i
        };
        ASSERT_TRUE(awaitApplyFinality(
            store_io_context,
            store,
            CHAIN_ID,
            heights,
            1'700'002'100'000 + i,
            2048));
    }

    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 0);
    events_sql::expectRowCount(paths.hot_db, "normalized_events_hot", 0);
    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 0);
    {
        SqliteReadonly db(paths.hot_db);
        EXPECT_EQ(db.scalarInt64("SELECT head_block FROM finality_state WHERE chain_id=1;"), 204);
    }
}
