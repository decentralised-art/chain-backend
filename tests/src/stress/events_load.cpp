#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    std::size_t readEnvSizeOrDefault(const char * env_name, const std::size_t default_value)
    {
        const char * raw = std::getenv(env_name);
        if(raw == nullptr || raw[0] == '\0')
        {
            return default_value;
        }

        std::size_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(raw, raw + std::strlen(raw), parsed, 10);
        if(ec != std::errc{} || ptr != raw + std::strlen(raw) || parsed == 0)
        {
            return default_value;
        }
        return parsed;
    }
}

TEST_F(StressTest, Stress_Events_SustainedIngestProjectionEnvelope)
{
    const std::size_t event_count = readEnvSizeOrDefault("DCN_EVENTS_STRESS_COUNT", 20'000);
    const std::size_t batch_size = readEnvSizeOrDefault("DCN_EVENTS_STRESS_BATCH", 500);

    const auto paths = makeTempEventsPaths("stress_events_envelope");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    std::int64_t next_block = 3'000;
    std::size_t emitted = 0;
    while(emitted < event_count)
    {
        const std::size_t this_batch = std::min(batch_size, event_count - emitted);
        std::vector<events::DecodedEvent> events_batch;
        std::vector<events::ChainBlockInfo> block_batch;
        events_batch.reserve(this_batch);
        block_batch.reserve(this_batch);

        for(std::size_t i = 0; i < this_batch; ++i)
        {
            const std::int64_t block = next_block + static_cast<std::int64_t>(i);
            const std::uint8_t b = static_cast<std::uint8_t>((emitted + i) % 255 + 1);

            events::DecodedEvent event = makeDecodedEvent(
                block,
                0,
                block,
                b,
                static_cast<std::uint8_t>((b + 31) % 255 + 1),
                events::EventType::CONNECTOR_ADDED,
                events::EventState::OBSERVED,
                1'700'010'000 + block,
                1'700'010'000'000 + block);
            events_batch.push_back(std::move(event));
        }

        for(const auto & event : events_batch)
        {
            block_batch.push_back(makeBlockInfo(
                event.raw.block_number,
                event.raw.block_hash,
                hexBytes(0x21, 32),
                event.raw.block_time.value_or(0),
                event.raw.seen_at_ms));
        }

        ASSERT_TRUE(awaitIngestBatch(
            store_io_context,
            store,
            CHAIN_ID,
            events_batch,
            block_batch,
            next_block + static_cast<std::int64_t>(this_batch),
            1'700'010'500'000 + emitted));
        emitted += this_batch;
        next_block += static_cast<std::int64_t>(this_batch);
    }

    const std::size_t projected = projectAll(store_io_context, store, feed_obj, 1'700'011'000'000);
    EXPECT_EQ(projected, event_count);

    const feed::StreamPage replay = feed_obj.getStreamPage(feed::StreamQuery{
        .since_seq = 0,
        .limit = std::min<std::size_t>(event_count, feed::MAX_STREAM_LIMIT)
    });
    EXPECT_FALSE(replay.deltas.empty());
    EXPECT_EQ(replay.deltas.front().stream_seq, 1);
    EXPECT_LT(replay.deltas.front().stream_seq, replay.deltas.back().stream_seq);
}
