#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <sqlite3.h>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    std::string sqlitePragmaText(const std::filesystem::path & db_path, const std::string & pragma_name)
    {
        sqlite3 * db = nullptr;
        const int open_rc = sqlite3_open_v2(
            db_path.string().c_str(),
            &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if(open_rc != SQLITE_OK)
        {
            if(db != nullptr)
            {
                sqlite3_close(db);
            }
            return {};
        }

        sqlite3_stmt * stmt = nullptr;
        const std::string sql = std::format("PRAGMA {};", pragma_name);
        if(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_close(db);
            return {};
        }

        std::string value;
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
            const unsigned char * txt = sqlite3_column_text(stmt, 0);
            if(txt != nullptr)
            {
                value = reinterpret_cast<const char *>(txt);
            }
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return value;
    }
}

TEST_F(UnitTest, Events_Maintenance_ConfiguresWalJournalMode)
{
    const auto paths = makeTempEventsPaths("maintenance_wal_mode");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const std::string journal_mode = sqlitePragmaText(paths.hot_db, "journal_mode");
    EXPECT_EQ(journal_mode, "wal");
}

TEST_F(UnitTest, Events_Maintenance_RejectsLikelyNetworkFilesystemPath)
{
    EXPECT_THROW(
        {
            asio::io_context store_io_context;
            events::SQLiteHotStore store("//server/share/events_hot.sqlite", CHAIN_ID);
        },
        std::runtime_error);
}

TEST_F(UnitTest, Events_Maintenance_WalCheckpointRunsAndReportsStats)
{
    const auto paths = makeTempEventsPaths("maintenance_wal_checkpoint");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent event =
        makeDecodedEvent(201, 0, 1, 0xAA, 0xBA, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'010'000);
    const events::ChainBlockInfo block =
        makeBlockInfo(201, event.raw.block_hash, hexBytes(0xA9, 32), 1'700'010'000, 1'700'010'000'100);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event}, {block}, 202, 1'700'010'000'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'010'000'300), 1u);

    const storage::sqlite::WalCheckpointStats passive = awaitCheckpointWalPassive(store_io_context, store);
    EXPECT_TRUE(passive.ok);
    EXPECT_GE(passive.log_frames, 0);
    EXPECT_GE(passive.checkpointed_frames, 0);

    const storage::sqlite::WalCheckpointStats truncate = awaitCheckpointWalTruncate(store_io_context, store);
    EXPECT_TRUE(truncate.ok);
    EXPECT_GE(truncate.wal_bytes, 0u);
}

TEST_F(UnitTest, Events_Prune_DeletesOnlyConsumedFinalizedBelowFloor)
{
    const auto paths = makeTempEventsPaths("prune_consumed_finalized_below_floor");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    // --- event1: block 100, finalized first → lower change_seq (the prunable target)
    const events::DecodedEvent event1 = makeDecodedEvent(
        100, 0, 1, 0xA1, 0xB1,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'600'000'000);
    const events::ChainBlockInfo block1 = makeBlockInfo(
        100, event1.raw.block_hash, hexBytes(0x01, 32), 1'600'000'000, 1'700'000'000'000);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event1}, {block1}, 101, 1'700'000'000'100));

    // Finalize block 100 => event1 transitions observed->safe->finalized, each with a new change_seq
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID,
        events::FinalityHeights{.head = 200, .safe = 100, .finalized = 100},
        1'700'000'000'200, 2048));

    // Capture event1's change_seq after finalization — this becomes the watermark
    std::int64_t watermark = 0;
    {
        SqliteReadonly db(paths.hot_db);
        watermark = db.scalarInt64(std::format(
            "SELECT change_seq FROM normalized_events_hot WHERE block_hash='{}' AND log_index=1;",
            event1.raw.block_hash));
    }
    ASSERT_GT(watermark, 0);

    // --- event2: block 50, finalized AFTER event1 → change_seq > watermark (lagging-projector case)
    const events::DecodedEvent event2 = makeDecodedEvent(
        50, 0, 1, 0xA2, 0xB2,
        events::EventType::TRANSFORMATION_ADDED,
        events::EventState::OBSERVED,
        1'500'000'000);
    const events::ChainBlockInfo block2 = makeBlockInfo(
        50, event2.raw.block_hash, hexBytes(0x02, 32), 1'500'000'000, 1'700'000'000'300);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event2}, {block2}, 51, 1'700'000'000'400));

    // Monotonic finality: effective_finalized stays 100; event2 (block 50<=100) also transitions
    // and gets a new change_seq > watermark
    ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID,
        events::FinalityHeights{.head = 200, .safe = 50, .finalized = 50},
        1'700'000'000'500, 2048));

    // --- event3: block 200, NOT finalized (stays 'observed'; effective_finalized=100 < 200)
    const events::DecodedEvent event3 = makeDecodedEvent(
        200, 0, 1, 0xA3, 0xB3,
        events::EventType::CONDITION_ADDED,
        events::EventState::OBSERVED,
        1'700'000'000);
    const events::ChainBlockInfo block3 = makeBlockInfo(
        200, event3.raw.block_hash, hexBytes(0x03, 32), 1'700'000'000, 1'700'000'000'600);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {event3}, {block3}, 201, 1'700'000'000'700));

    // Verify event2 has higher change_seq than the watermark (it was finalized after event1)
    {
        SqliteReadonly db(paths.hot_db);
        const std::int64_t event2_seq = db.scalarInt64(std::format(
            "SELECT change_seq FROM normalized_events_hot WHERE block_hash='{}' AND log_index=1;",
            event2.raw.block_hash));
        ASSERT_GT(event2_seq, watermark);
    }

    // Prune: watermark = event1's change_seq, finalized_floor_block = 200 (covers all blocks)
    const std::size_t pruned = store.pruneConsumedRaw("local", watermark, 200, 100);
    EXPECT_EQ(pruned, 1u);  // only event1 qualifies

    // event1 must be gone from BOTH tables
    {
        SqliteReadonly db(paths.hot_db);
        EXPECT_EQ(db.scalarInt64(std::format(
            "SELECT COUNT(1) FROM normalized_events_hot WHERE block_hash='{}' AND log_index=1;",
            event1.raw.block_hash)), 0)
            << "event1 normalized row should have been pruned";
        EXPECT_EQ(db.scalarInt64(std::format(
            "SELECT COUNT(1) FROM raw_events_hot WHERE block_hash='{}' AND log_index=1;",
            event1.raw.block_hash)), 0)
            << "event1 raw row should have been pruned";
    }

    // event2 must remain: change_seq > watermark; a lagging projector holds the watermark back
    {
        SqliteReadonly db(paths.hot_db);
        EXPECT_EQ(db.scalarInt64(std::format(
            "SELECT COUNT(1) FROM normalized_events_hot WHERE block_hash='{}' AND log_index=1;",
            event2.raw.block_hash)), 1)
            << "event2 normalized row should remain (change_seq above watermark)";
        EXPECT_EQ(db.scalarInt64(std::format(
            "SELECT COUNT(1) FROM raw_events_hot WHERE block_hash='{}' AND log_index=1;",
            event2.raw.block_hash)), 1)
            << "event2 raw row should remain (change_seq above watermark)";
    }

    // event3 must remain: not finalized (state='observed')
    {
        SqliteReadonly db(paths.hot_db);
        EXPECT_EQ(db.scalarInt64(std::format(
            "SELECT COUNT(1) FROM normalized_events_hot WHERE block_hash='{}' AND log_index=1;",
            event3.raw.block_hash)), 1)
            << "event3 normalized row should remain (not finalized)";
        EXPECT_EQ(db.scalarInt64(std::format(
            "SELECT COUNT(1) FROM raw_events_hot WHERE block_hash='{}' AND log_index=1;",
            event3.raw.block_hash)), 1)
            << "event3 raw row should remain (not finalized)";
    }
}
