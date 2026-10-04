#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <algorithm>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    void ingestAndProjectOne(
        asio::io_context & writer_io_context,
        events::SQLiteHotStore & store,
        feed::Feed & feed,
        const std::int64_t block_number,
        const std::uint8_t block_hash_byte,
        const std::uint8_t tx_hash_byte,
        const std::int64_t block_time,
        const std::int64_t now_ms)
    {
        const events::DecodedEvent event = makeDecodedEvent(
            block_number,
            0,
            1,
            block_hash_byte,
            tx_hash_byte,
            events::EventType::CONNECTOR_ADDED,
            events::EventState::OBSERVED,
            block_time,
            now_ms - 50);
        const events::ChainBlockInfo block = makeBlockInfo(
            block_number,
            event.raw.block_hash,
            hexBytes(static_cast<std::uint8_t>(block_hash_byte - 1), 32),
            block_time,
            now_ms - 40);
        ASSERT_TRUE(awaitIngestBatch(
            writer_io_context,
            store,
            CHAIN_ID,
            {event},
            {block},
            block_number + 1,
            now_ms - 30));
        EXPECT_EQ(projectAll(writer_io_context, store, feed, now_ms), 1u);
    }
}

TEST_F(UnitTest, Events_StreamPage_SinceSeqAndOrdering_AreMonotonic)
{
    const auto paths = makeTempEventsPaths("stream_monotonic");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    ingestAndProjectOne(store_io_context, store, feed_obj, 80, 0xD0, 0xF0, 1'700'000'700, 1'700'000'700'100);
    ingestAndProjectOne(store_io_context, store, feed_obj, 81, 0xD1, 0xF1, 1'700'000'701, 1'700'000'701'100);
    ingestAndProjectOne(store_io_context, store, feed_obj, 82, 0xD2, 0xF2, 1'700'000'702, 1'700'000'702'100);

    const feed::StreamPage page = feed_obj.getStreamPage(feed::StreamQuery{
        .since_seq = 1,
        .limit = 100
    });

    ASSERT_EQ(page.deltas.size(), 2u);
    EXPECT_EQ(page.deltas.at(0).stream_seq, 2);
    EXPECT_EQ(page.deltas.at(1).stream_seq, 3);
    ASSERT_TRUE(page.last_seq.has_value());
    EXPECT_EQ(*page.last_seq, 3);
    EXPECT_FALSE(page.has_more);
}

TEST_F(UnitTest, Events_StreamPage_ServesEverySourceInStreamOrder)
{
    const auto paths = makeTempEventsPaths("stream_chain_filter");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    const events::DecodedEvent chain_one_event = makeDecodedEvent(
        300,
        0,
        1,
        0xA0,
        0xB0,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'050'000,
        1'700'050'000'000);
    const events::ChainBlockInfo chain_one_block = makeBlockInfo(
        300,
        chain_one_event.raw.block_hash,
        hexBytes(0x50, 32),
        1'700'050'000,
        1'700'050'000'100);
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, CHAIN_ID, {chain_one_event}, {chain_one_block}, 301, 1'700'050'000'200));

    const events::DecodedEvent chain_two_event = makeDecodedEvent(
        301,
        0,
        1,
        0xA1,
        0xB1,
        events::EventType::TRANSFORMATION_ADDED,
        events::EventState::OBSERVED,
        1'700'050'001,
        1'700'050'001'000);
    events::ChainBlockInfo chain_two_block = makeBlockInfo(
        301,
        chain_two_event.raw.block_hash,
        hexBytes(0x51, 32),
        1'700'050'001,
        1'700'050'001'100);
    chain_two_block.chain_id = 2;
    ASSERT_TRUE(awaitIngestBatch(store_io_context, store, 2, {chain_two_event}, {chain_two_block}, 302, 1'700'050'001'200));

    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'050'001'300), 2u);

    const feed::StreamPage page = feed_obj.getStreamPage(feed::StreamQuery{
        .since_seq = 0,
        .limit = 16
    });
    // Deltas from every source share one stream, in projection order; each keeps its
    // own chain in its id.
    ASSERT_EQ(page.deltas.size(), 2u);
    EXPECT_EQ(page.deltas[0].feed_id.rfind("local:1:", 0), 0u);
    EXPECT_EQ(page.deltas[1].feed_id.rfind("local:2:", 0), 0u);
    EXPECT_LT(page.deltas[0].stream_seq, page.deltas[1].stream_seq);
}

TEST_F(UnitTest, Events_StreamPage_ReconnectWithLastSeq_DoesNotDuplicate)
{
    const auto paths = makeTempEventsPaths("stream_reconnect");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    ingestAndProjectOne(store_io_context, store, feed_obj, 83, 0xD3, 0xF3, 1'700'000'703, 1'700'000'703'100);
    ingestAndProjectOne(store_io_context, store, feed_obj, 84, 0xD4, 0xF4, 1'700'000'704, 1'700'000'704'100);

    const feed::StreamPage first = feed_obj.getStreamPage(feed::StreamQuery{
        .since_seq = 0,
        .limit = 100
    });
    ASSERT_EQ(first.deltas.size(), 2u);
    ASSERT_TRUE(first.last_seq.has_value());

    const feed::StreamPage second = feed_obj.getStreamPage(feed::StreamQuery{
        .since_seq = *first.last_seq,
        .limit = 100
    });
    EXPECT_TRUE(second.deltas.empty());
    EXPECT_FALSE(second.last_seq.has_value());
}

// NOTE: This test is in the known-failing list (feed_repo wiring to SSE not
// completed) but must compile cleanly.
TEST_F(UnitTest, Events_API_FeedStream_EmitsSseFramesWithFloorAndMeta)
{
    const auto paths = makeTempEventsPaths("stream_sse_contract");
    asio::io_context writer_io_context;
    events::SQLiteHotStore writer(paths.hot_db);
    // retention=10ms: outbox rows pruned quickly on next projectBatch pass
    feed::Feed feed_obj(writer_io_context, paths.feed_db, paths.feed_archive_root, 10);

    ingestAndProjectOne(writer_io_context, writer, feed_obj, 85, 0xD5, 0xF5, 1'700'000'705, 1'700'000'705'100);
    ingestAndProjectOne(writer_io_context, writer, feed_obj, 86, 0xD6, 0xF6, 1'700'000'706, 1'700'000'706'100);

    const std::string body = buildFeedStreamSseReplay(feed_obj, feed::StreamQuery{.since_seq = 0, .limit = 10});

    const std::vector<SseFrame> frames = parseSseFrames(body);
    ASSERT_GE(frames.size(), 3u);
    EXPECT_TRUE(frames.front().is_comment);
    EXPECT_EQ(frames.front().comment, "min_available_seq=2");

    ASSERT_TRUE(frames.at(1).id.has_value());
    EXPECT_EQ(*frames.at(1).id, 2);
    EXPECT_EQ(frames.at(1).event, "connector_added");
    const auto first_data = json::parse(frames.at(1).data, nullptr, false);
    ASSERT_FALSE(first_data.is_discarded());
    EXPECT_EQ(first_data.value("stream_seq", 0), 2);
    EXPECT_EQ(first_data.value("event_type", std::string{}), "connector_added");
    EXPECT_FALSE(first_data.contains("op"));

    const SseFrame & meta_frame = frames.back();
    EXPECT_EQ(meta_frame.event, "stream_meta");
    const auto meta_data = json::parse(meta_frame.data, nullptr, false);
    ASSERT_FALSE(meta_data.is_discarded());
    EXPECT_EQ(meta_data.value("has_more", true), false);
    EXPECT_EQ(meta_data["last_seq"], 2);
    EXPECT_EQ(meta_data.value("requested_since_seq", -1), 0);
    EXPECT_EQ(meta_data.value("min_available_seq", -1), 2);
    EXPECT_EQ(meta_data.value("replay_floor_seq", -1), 2);
    EXPECT_EQ(meta_data.value("stale_since_seq", true), false);
}

// NOTE: This test is in the known-failing list.
TEST_F(UnitTest, Events_API_FeedStream_AfterRestart_UsesDurableOutboxState)
{
    const auto paths = makeTempEventsPaths("stream_restart");
    {
        asio::io_context writer_io_context;
        events::SQLiteHotStore writer(paths.hot_db);
        feed::Feed feed_writer(writer_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);
        ingestAndProjectOne(writer_io_context, writer, feed_writer, 87, 0xD7, 0xF7, 1'700'000'707, 1'700'000'707'100);
        ingestAndProjectOne(writer_io_context, writer, feed_writer, 88, 0xD8, 0xF8, 1'700'000'708, 1'700'000'708'100);
    }

    // "Restart": open a new Feed on the same DB to verify durable state.
    asio::io_context io_context;
    feed::Feed feed_reader(io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);
    const std::string body = buildFeedStreamSseReplay(feed_reader, feed::StreamQuery{.since_seq = 1, .limit = 10});

    const std::vector<SseFrame> frames = parseSseFrames(body);
    ASSERT_GE(frames.size(), 2u);

    const auto delta_it = std::find_if(
        frames.begin(),
        frames.end(),
        [](const SseFrame & frame)
        {
            return frame.event == "connector_added";
        });
    ASSERT_NE(delta_it, frames.end());
    ASSERT_TRUE(delta_it->id.has_value());
    EXPECT_EQ(*delta_it->id, 2);
}

TEST_F(UnitTest, Events_API_FeedStream_StaleCursor_IsStructuredInMeta)
{
    const auto paths = makeTempEventsPaths("stream_stale_cursor_meta");
    {
        asio::io_context writer_io_context;
        events::SQLiteHotStore writer(paths.hot_db);
        // retention=10ms: first outbox row expires on the next projectBatch call
        feed::Feed feed_obj(writer_io_context, paths.feed_db, paths.feed_archive_root, 10);
        ingestAndProjectOne(writer_io_context, writer, feed_obj, 89, 0xD9, 0xF9, 1'700'000'709, 1'700'000'709'100);
        // Force a projectBatch pass at a later timestamp so maintainOutbox prunes the first row
        EXPECT_EQ(awaitProjectBatch(writer_io_context, writer, feed_obj, 16, 1'700'000'710'500), 0u);
        ingestAndProjectOne(writer_io_context, writer, feed_obj, 90, 0xDA, 0xFA, 1'700'000'710, 1'700'000'710'100);
    }

    // "Restart" on the same feed DB.
    asio::io_context io_context;
    feed::Feed feed_reader(io_context, paths.feed_db, paths.feed_archive_root, 10);
    const std::string body = buildFeedStreamSseReplay(feed_reader, feed::StreamQuery{.since_seq = 1, .limit = 10});

    const std::vector<SseFrame> frames = parseSseFrames(body);
    ASSERT_GE(frames.size(), 2u);
    const auto meta_frame_it = std::find_if(
        frames.begin(),
        frames.end(),
        [](const SseFrame & frame)
        {
            return frame.event == "stream_meta";
        });
    ASSERT_NE(meta_frame_it, frames.end());

    const auto meta = json::parse(meta_frame_it->data, nullptr, false);
    ASSERT_FALSE(meta.is_discarded());
    EXPECT_EQ(meta.value("requested_since_seq", -1), 1);
    EXPECT_EQ(meta.value("stale_since_seq", false), true);
    EXPECT_GE(meta.value("min_available_seq", 0), 2);
    EXPECT_GE(meta.value("replay_floor_seq", 0), 2);
}

TEST_F(UnitTest, Events_API_Feed_RejectsZeroAndOversizedLimit)
{
    const auto paths = makeTempEventsPaths("feed_limit_bounds");
    asio::io_context io_context;
    feed::Feed feed_obj(io_context, paths.feed_db, paths.feed_archive_root, 10);

    for(const std::size_t limit : {std::size_t{0}, std::size_t{257}})
    {
        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintQueryArg(limit));

        const auto response = runAwaitable(io_context, GET_feed(http::Request{}, {}, std::move(query_args), feed_obj));
        EXPECT_EQ(response.getCode(), http::Code::BadRequest) << "limit=" << limit;
        const auto body = json::parse(response.getBody(), nullptr, false);
        ASSERT_FALSE(body.is_discarded());
        EXPECT_EQ(body.value("message", std::string{}), "Invalid argument limit. limit error: Out of range.");
    }

    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintQueryArg(1));
    const auto response = runAwaitable(io_context, GET_feed(http::Request{}, {}, std::move(query_args), feed_obj));
    ASSERT_EQ(response.getCode(), http::Code::OK);
    EXPECT_EQ(json::parse(response.getBody()).value("limit", 0), 1);
}
