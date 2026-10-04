#include <filesystem>

#include "unit-tests.hpp"

#include "events_test_harness.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    void finalizeAndProject(
        asio::io_context & store_io_context,
        events::SQLiteHotStore & store,
        feed::Feed & feed,
        const events::DecodedEvent & event,
        const std::int64_t now_ms)
    {
        const events::ChainBlockInfo block = makeBlockInfo(
            event.raw.block_number,
            event.raw.block_hash,
            hexBytes(0x60, 32),
            event.raw.block_time.value_or(0),
            now_ms - 20);
        ASSERT_TRUE(awaitIngestBatch(
            store_io_context,
            store,
            CHAIN_ID,
            {event},
            {block},
            event.raw.block_number + 1,
            now_ms - 10));
        EXPECT_EQ(projectAll(store_io_context, store, feed, now_ms), 1u);

        const events::FinalityHeights heights{
            .head = event.raw.block_number + 100,
            .safe = event.raw.block_number,
            .finalized = event.raw.block_number
        };
        ASSERT_TRUE(awaitApplyFinality(store_io_context, store, CHAIN_ID, heights, now_ms + 10, 2048));
        EXPECT_EQ(projectAll(store_io_context, store, feed, now_ms + 20), 1u);
    }

    std::filesystem::path readyShardPath(const std::filesystem::path & feed_db)
    {
        SqliteReadonly fdb(feed_db);
        return std::filesystem::path(
            fdb.scalarText("SELECT path FROM shard_catalog WHERE state='READY' ORDER BY archive_month DESC LIMIT 1;"));
    }

    // A consolidated shard is a single file: the main db exists and no WAL/SHM/rollback
    // sidecars linger at rest.
    void expectSingleFileShard(const std::filesystem::path & shard)
    {
        ASSERT_FALSE(shard.empty());
        EXPECT_TRUE(std::filesystem::exists(shard));
        EXPECT_FALSE(std::filesystem::exists(shard.string() + "-wal"));
        EXPECT_FALSE(std::filesystem::exists(shard.string() + "-shm"));
        EXPECT_FALSE(std::filesystem::exists(shard.string() + "-journal"));
    }
}

// Export must leave the shard as a single rollback-journal file, not a WAL trio.
TEST_F(UnitTest, Feed_ArchiveShard_ExportProducesSingleFileNoWalShm)
{
    const auto paths = makeTempEventsPaths("archive_shard_single_file");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(
        500, 0, 1, 0xC0, 0x50, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'001'000);
    finalizeAndProject(store_io_context, store, feed_obj, event, 1'700'001'000'200);

    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 36500, 1'700'001'100'000));

    const std::filesystem::path shard = readyShardPath(paths.feed_db);
    expectSingleFileShard(shard);

    SqliteReadonly shard_db(shard);
    EXPECT_EQ(shard_db.scalarText("PRAGMA journal_mode;"), "delete");
    EXPECT_EQ(shard_db.scalarInt64("SELECT COUNT(1) FROM feed_items_archive;"), 1);
}

// Re-exporting into an already-existing shard (in-place upsert) must not resurrect a WAL.
TEST_F(UnitTest, Feed_ArchiveShard_ReExportKeepsSingleFile)
{
    const auto paths = makeTempEventsPaths("archive_shard_reexport");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(
        510, 0, 1, 0xC1, 0x51, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'001'000);
    finalizeAndProject(store_io_context, store, feed_obj, event, 1'700'001'000'200);

    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 36500, 1'700'001'100'000));
    const std::filesystem::path shard = readyShardPath(paths.feed_db);
    expectSingleFileShard(shard);

    // Force the feed row back to exported=0 so the next cycle re-opens and upserts the same shard.
    {
        SqliteWritable fdb(paths.feed_db);
        fdb.exec("UPDATE feed_items_hot SET exported=0 WHERE chain_id=1;");
    }
    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 36500, 1'700'001'200'000));

    expectSingleFileShard(shard);
    {
        SqliteReadonly fdb(paths.feed_db);
        EXPECT_EQ(fdb.scalarInt64("SELECT COUNT(1) FROM shard_catalog WHERE state='READY';"), 1);
    }
    SqliteReadonly shard_db(shard);
    EXPECT_EQ(shard_db.scalarInt64("SELECT COUNT(1) FROM feed_items_archive;"), 1);
}

// Reading the archive as the sole source must return the row and leave no sidecars behind
// (read connections are read-only and cannot consolidate, so nothing may be created).
TEST_F(UnitTest, Feed_ArchiveShard_ReadAsSoleSourceLeavesNoSidecars)
{
    const auto paths = makeTempEventsPaths("archive_shard_read_sole_source");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 60*60*1000);

    const events::DecodedEvent event = makeDecodedEvent(
        520, 0, 1, 0xC2, 0x52, events::EventType::CONDITION_ADDED, events::EventState::OBSERVED, 1'700'001'000);
    finalizeAndProject(store_io_context, store, feed_obj, event, 1'700'001'000'200);

    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 36500, 1'700'001'100'000));
    const std::filesystem::path shard = readyShardPath(paths.feed_db);
    expectSingleFileShard(shard);

    // Drop the hot copy so the archive shard is the only place the row lives.
    {
        SqliteWritable fdb(paths.feed_db);
        fdb.exec("DELETE FROM feed_items_hot WHERE chain_id=1;");
    }

    const feed::FeedPage page = feed_obj.getFeedPage(feed::FeedQuery{.limit = 10});
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items[0].event_type, std::string(events::CONDITION_ADDED_TYPE));

    // The read-only fan-out must not have created WAL/SHM/journal files.
    expectSingleFileShard(shard);
}
