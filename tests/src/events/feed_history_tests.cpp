#include "unit-tests.hpp"

#include "events_sql_assertions.hpp"
#include "events_test_harness.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    void ingestProjectFinalize(
        asio::io_context & store_io_context,
        events::SQLiteHotStore & store,
        feed::Feed & feed,
        const events::DecodedEvent & event,
        const std::int64_t now_ms,
        const std::string & source_id = "local",
        const std::uint64_t chain_id = CHAIN_ID)
    {
        events::ChainBlockInfo block = makeBlockInfo(
            event.raw.block_number,
            event.raw.block_hash,
            hexBytes(0x70, 32),
            event.raw.block_time.value_or(0),
            now_ms - 30);
        block.chain_id = chain_id;
        ASSERT_TRUE(awaitIngestBatchForSource(
            store_io_context,
            store,
            source_id,
            {event},
            {block},
            static_cast<std::uint64_t>(event.raw.block_number + 1),
            now_ms - 20,
            chain_id));
        EXPECT_EQ(projectAll(store_io_context, store, feed, now_ms - 10), 1u);

        ASSERT_TRUE(awaitApplyFinalityForSource(
            store_io_context,
            store,
            source_id,
            event.raw.block_number + 100,
            event.raw.block_number,
            event.raw.block_number,
            now_ms,
            chain_id));
        EXPECT_EQ(projectAll(store_io_context, store, feed, now_ms + 10), 1u);
    }
}

TEST_F(UnitTest, Events_History_HotFeedRead_PrefersHotRows)
{
    const auto paths = makeTempEventsPaths("history_hot_first");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent first = makeDecodedEvent(90, 0, 1, 0xE0, 0x20, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'800);
    const events::DecodedEvent second = makeDecodedEvent(91, 0, 1, 0xE1, 0x21, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'801);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {first, second},
        {
            makeBlockInfo(90, first.raw.block_hash, hexBytes(0x6F, 32), 1'700'000'800, 1'700'000'800'100),
            makeBlockInfo(91, second.raw.block_hash, hexBytes(0x70, 32), 1'700'000'801, 1'700'000'801'100)
        },
        92,
        1'700'000'801'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'801'300), 2u);

    const feed::FeedPage page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 10,
        .include_unfinalized = true
    });
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items.at(0).block_number, 91);
    EXPECT_EQ(page.items.at(1).block_number, 90);
}

TEST_F(UnitTest, Events_History_HotFeedRead_ServesEverySourceInOnePage)
{
    const auto paths = makeTempEventsPaths("history_chain_filter");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent chain_one_event = makeDecodedEvent(
        100,
        0,
        1,
        0xEE,
        0x2E,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'100'000);
    events::ChainBlockInfo chain_one_block = makeBlockInfo(
        100,
        chain_one_event.raw.block_hash,
        hexBytes(0x6D, 32),
        1'700'100'000,
        1'700'100'000'100);

    const events::DecodedEvent chain_two_event = makeDecodedEvent(
        200,
        0,
        1,
        0xEF,
        0x2F,
        events::EventType::TRANSFORMATION_ADDED,
        events::EventState::OBSERVED,
        1'700'200'000);
    events::ChainBlockInfo chain_two_block = makeBlockInfo(
        200,
        chain_two_event.raw.block_hash,
        hexBytes(0x6C, 32),
        1'700'200'000,
        1'700'200'000'100);
    chain_two_block.chain_id = 2;

    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {chain_one_event},
        {chain_one_block},
        101,
        1'700'100'000'200));
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        2,
        {chain_two_event},
        {chain_two_block},
        201,
        1'700'200'000'200));

    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'200'000'300), 2u);

    // Both rows were projected at the same time, so block number orders them; each
    // keeps its own chain in its id.
    const feed::FeedPage page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 10,
        .include_unfinalized = true
    });
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].block_number, 200);
    EXPECT_EQ(page.items[0].feed_id.rfind("local:2:", 0), 0u);
    EXPECT_EQ(page.items[1].block_number, 100);
    EXPECT_EQ(page.items[1].feed_id.rfind("local:1:", 0), 0u);

    // A cursor taken on one chain's item pages on into the other chain's items.
    const feed::FeedPage first = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 1,
        .include_unfinalized = true
    });
    ASSERT_EQ(first.items.size(), 1u);
    ASSERT_TRUE(first.next_before_cursor.has_value());

    const feed::FeedPage second = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 1,
        .before_cursor = first.next_before_cursor,
        .include_unfinalized = true
    });
    ASSERT_EQ(second.items.size(), 1u);
    EXPECT_EQ(second.items.front().feed_id, page.items[1].feed_id);
    EXPECT_FALSE(second.has_more);
}

TEST_F(UnitTest, Events_History_DedupesAcrossHotAndArchive_ByFeedId)
{
    const auto paths = makeTempEventsPaths("history_dedupe_hot_archive");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent finalized = makeDecodedEvent(92, 0, 1, 0xE2, 0x22, events::EventType::CONDITION_ADDED, events::EventState::OBSERVED, 1'700'000'802);
    ingestProjectFinalize(store_io_context, store, feed_obj, finalized, 1'700'000'802'200);

    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 36500, 1'700'000'900'000));

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
    {
        SqliteReadonly fdb(paths.feed_db);
        EXPECT_EQ(fdb.scalarInt64("SELECT COUNT(1) FROM shard_catalog WHERE state='READY';"), 1);
    }

    const feed::FeedPage page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 10,
        .include_unfinalized = false
    });
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items.front().status, "finalized");
}

// Every chain's rows are archived and read back; a cursor taken on one chain's archived
// row pages on into another chain's, whatever their block numbers.
TEST_F(UnitTest, Events_History_ArchiveServesEveryChainAcrossCursorPages)
{
    const auto paths = makeTempEventsPaths("history_archive_every_chain");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent chain_one = makeDecodedEvent(
        100, 0, 1, 0xA0, 0xB0, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'000);
    ingestProjectFinalize(store_io_context, store, feed_obj, chain_one, 1'700'000'000'500);

    const events::DecodedEvent chain_two = makeDecodedEvent(
        5'000'000, 0, 1, 0xA1, 0xB1, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'100);
    ingestProjectFinalize(store_io_context, store, feed_obj, chain_two, 1'700'000'100'500, "second_chain", 2);

    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 0, 1'900'000'000'000));
    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 0);
    {
        SqliteReadonly fdb(paths.feed_db);
        EXPECT_EQ(fdb.scalarInt64("SELECT COUNT(DISTINCT chain_id) FROM shard_catalog WHERE state='READY';"), 2);
    }

    // Chain two's row was projected later, so it comes first.
    const feed::FeedPage first = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 1,
        .include_unfinalized = false
    });
    ASSERT_EQ(first.items.size(), 1u);
    EXPECT_EQ(first.items.front().feed_id.rfind("local:2:", 0), 0u);
    ASSERT_TRUE(first.next_before_cursor.has_value());

    const feed::FeedPage second = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 1,
        .before_cursor = first.next_before_cursor,
        .include_unfinalized = false
    });
    ASSERT_EQ(second.items.size(), 1u);
    EXPECT_EQ(second.items.front().feed_id.rfind("local:1:", 0), 0u);
    EXPECT_FALSE(second.has_more);
}

TEST_F(UnitTest, Events_History_OrderingAcrossHotArchiveBoundary_IsCanonical)
{
    const auto paths = makeTempEventsPaths("history_boundary_order");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent older = makeDecodedEvent(93, 0, 1, 0xE3, 0x23, events::EventType::CONNECTOR_ADDED, events::EventState::OBSERVED, 1'700'000'803);
    ingestProjectFinalize(store_io_context, store, feed_obj, older, 1'700'000'803'200);
    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 0, 1'900'000'000'000));

    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 0);

    const events::DecodedEvent newer = makeDecodedEvent(120, 0, 1, 0xE4, 0x24, events::EventType::TRANSFORMATION_ADDED, events::EventState::OBSERVED, 1'700'000'900);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {newer},
        {makeBlockInfo(120, newer.raw.block_hash, hexBytes(0x6E, 32), 1'700'000'900, 1'700'000'900'100)},
        121,
        1'700'000'900'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'000'900'300), 1u);

    const feed::FeedPage page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 10,
        .include_unfinalized = true
    });
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items.at(0).block_number, 120);
    EXPECT_EQ(page.items.at(1).block_number, 93);
}

TEST_F(UnitTest, Events_History_OrderingUsesCreatedAtMsAndCursorPaginatesByIt)
{
    const auto paths = makeTempEventsPaths("history_updated_at_order");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent high_block_first = makeDecodedEvent(
        200,
        0,
        1,
        0xF0,
        0x30,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'300'000);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {high_block_first},
        {makeBlockInfo(200, high_block_first.raw.block_hash, hexBytes(0x71, 32), 1'700'300'000, 1'700'300'000'100)},
        201,
        1'700'300'000'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'300'000'300), 1u);

    const events::DecodedEvent low_block_later = makeDecodedEvent(
        100,
        0,
        1,
        0xF1,
        0x31,
        events::EventType::TRANSFORMATION_ADDED,
        events::EventState::OBSERVED,
        1'700'300'100);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {low_block_later},
        {makeBlockInfo(100, low_block_later.raw.block_hash, hexBytes(0x72, 32), 1'700'300'100, 1'700'300'100'100)},
        201,
        1'700'300'100'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'300'100'300), 1u);

    const feed::FeedPage ordered_page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 10,
        .include_unfinalized = true
    });
    ASSERT_EQ(ordered_page.items.size(), 2u);
    EXPECT_EQ(ordered_page.items.at(0).block_number, 100);
    EXPECT_EQ(ordered_page.items.at(1).block_number, 200);
    EXPECT_GT(ordered_page.items.at(0).created_at_ms, ordered_page.items.at(1).created_at_ms);

    const parse::Result<feed::CursorKey> parsed_cursor = feed::parseHistoryCursor(ordered_page.items.at(0).history_cursor);
    ASSERT_TRUE(parsed_cursor.has_value());
    EXPECT_EQ(parsed_cursor->chain_namespace, "local");
    EXPECT_EQ(parsed_cursor->created_at_ms, ordered_page.items.at(0).created_at_ms);

    const feed::FeedPage first_page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 1,
        .include_unfinalized = true
    });
    ASSERT_EQ(first_page.items.size(), 1u);
    ASSERT_TRUE(first_page.next_before_cursor.has_value());
    EXPECT_EQ(first_page.items.front().block_number, 100);

    const feed::FeedPage second_page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 10,
        .before_cursor = first_page.next_before_cursor,
        .include_unfinalized = true
    });
    ASSERT_EQ(second_page.items.size(), 1u);
    EXPECT_EQ(second_page.items.front().block_number, 200);
}

TEST_F(UnitTest, Events_History_OrderingReadsArchiveEvenWhenHotExceedsLimit)
{
    const auto paths = makeTempEventsPaths("history_archive_scan_with_hot_limit");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const std::int64_t archived_projected_at_ms = 1'700'400'000'200;
    const std::int64_t hot_projected_before_archive_ms = archived_projected_at_ms - 1000;

    const events::DecodedEvent archived_top = makeDecodedEvent(
        50,
        0,
        1,
        0xF2,
        0x32,
        events::EventType::CONDITION_ADDED,
        events::EventState::OBSERVED,
        1'700'400'000);
    ingestProjectFinalize(store_io_context, store, feed_obj, archived_top, archived_projected_at_ms);
    ASSERT_TRUE(awaitRunArchiveCycle(store_io_context, feed_obj, 0, 1'900'000'000'000));

    const events::DecodedEvent hot_one = makeDecodedEvent(
        300,
        0,
        1,
        0xF3,
        0x33,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'500'000);
    const events::DecodedEvent hot_two = makeDecodedEvent(
        301,
        0,
        1,
        0xF4,
        0x34,
        events::EventType::TRANSFORMATION_ADDED,
        events::EventState::OBSERVED,
        1'700'500'001);
    ASSERT_TRUE(awaitIngestBatch(
        store_io_context,
        store,
        CHAIN_ID,
        {hot_one, hot_two},
        {
            makeBlockInfo(300, hot_one.raw.block_hash, hexBytes(0x73, 32), 1'700'500'000, 1'700'500'000'100),
            makeBlockInfo(301, hot_two.raw.block_hash, hexBytes(0x74, 32), 1'700'500'001, 1'700'500'001'100)
        },
        302,
        1'700'500'001'200));
    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, hot_projected_before_archive_ms), 2u);

    const feed::FeedPage page = feed_obj.getFeedPage(feed::FeedQuery{
        .limit = 1,
        .include_unfinalized = true
    });
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items.front().block_number, 50);
}
