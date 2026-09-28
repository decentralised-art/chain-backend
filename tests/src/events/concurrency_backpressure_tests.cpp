#include "unit-tests.hpp"

#include "events_sql_assertions.hpp"
#include "events_test_harness.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include <spdlog/spdlog.h>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    template<class Fn>
    auto runOnStrand(
        asio::io_context & io_context,
        asio::strand<asio::io_context::executor_type> & strand,
        Fn fn)
    {
        using ReturnT = std::invoke_result_t<Fn>;

        auto promise = std::make_shared<std::promise<ReturnT>>();
        auto future = promise->get_future();

        asio::post(strand, [promise, fn = std::move(fn)]() mutable
        {
            try
            {
                if constexpr(std::is_void_v<ReturnT>)
                {
                    fn();
                    promise->set_value();
                }
                else
                {
                    promise->set_value(fn());
                }
            }
            catch(...)
            {
                promise->set_exception(std::current_exception());
            }
        });

        io_context.restart();
        io_context.run();
        if constexpr(std::is_void_v<ReturnT>)
        {
            future.get();
        }
        else
        {
            return future.get();
        }
    }

    // Scripted EmittedLogRecord source: replays a fixed batch, advancing the
    // cursor by seq. One source owns one chain_id (the ingestion model).
    class FakeEmittedLogSource final : public events::IEmittedLogSource
    {
        public:
            FakeEmittedLogSource(std::uint64_t chain_id, std::vector<evm::EVM::EmittedLogRecord> records)
                : _chain_id(chain_id)
                , _source_id(std::format("fake:{}", chain_id))
                , _records(std::move(records))
            {
            }

            std::string_view sourceId() const override { return _source_id; }
            std::uint64_t chainId() const override { return _chain_id; }
            std::string_view chainNamespace() const override { return events::LOCAL_CHAIN_NAMESPACE; }
            events::CursorKind cursorKind() const override { return events::CursorKind::LOCAL_SEQUENCE; }
            bool ephemeralEntities() const override { return false; }

            asio::awaitable<events::SourcePoll> pollSince(std::uint64_t cursor, std::size_t limit) override
            {
                std::vector<evm::EVM::EmittedLogRecord> out;
                std::int64_t max_block = 0;
                for(const auto & record : _records)
                {
                    max_block = std::max<std::int64_t>(max_block, record.block_number);
                    if(record.seq >= cursor && out.size() < limit)
                    {
                        out.push_back(record);
                    }
                }
                const std::uint64_t next_cursor = out.empty() ? cursor : out.back().seq + 1;
                co_return events::SourcePoll{
                    std::move(out),
                    events::FinalityHeights{ .head = max_block, .safe = max_block, .finalized = max_block },
                    next_cursor
                };
            }

        private:
            std::uint64_t _chain_id;
            std::string _source_id;
            std::vector<evm::EVM::EmittedLogRecord> _records;
    };

    evm::EVM::EmittedLogRecord makeEmittedRecord(
        const std::uint64_t seq,
        const std::int64_t block_number,
        const std::uint8_t block_hash_byte)
    {
        evm::EVM::EmittedLogRecord record{};
        record.seq = seq;
        record.block_number = block_number;
        record.block_hash = hexBytes(block_hash_byte, 32);
        record.parent_hash = hexBytes(0x7F, 32);
        record.tx_index = 0;
        record.tx_hash = hexBytes(0x22, 32);
        record.log_index = 0;
        record.address = hexAddress(0xAB);
        record.topics = { topicForEvent("TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)") };
        record.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41),
            "fake_entity",
            makeAddressFromByte(0x43),
            makeAddressFromByte(0x42),
            2);
        record.block_time = 1'700'000'000;
        return record;
    }
}

TEST_F(UnitTest, Events_Concurrency_StrandSerializedIngestAndProject_AreDeterministic)
{
    const auto paths = makeTempEventsPaths("concurrency_bound_strand_marshal");
    asio::io_context io_context;
    auto hot_write_strand = asio::make_strand(io_context);
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent event = makeDecodedEvent(
        1'000,
        0,
        1,
        0x11,
        0x81,
        events::EventType::CONNECTOR_ADDED,
        events::EventState::OBSERVED,
        1'700'003'000,
        1'700'003'000'000);
    const events::ChainBlockInfo block = makeBlockInfo(
        1'000,
        event.raw.block_hash,
        hexBytes(0x10, 32),
        1'700'003'000,
        1'700'003'000'100);

    EXPECT_TRUE(awaitIngestBatch(io_context, store, CHAIN_ID, {event}, {block}, 1'001, 1'700'003'000'200));
    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 1);

    const bool accepted = runOnStrand(
        io_context,
        hot_write_strand,
        [&]() -> bool
        {
            return store.ingestBatch("local", CHAIN_ID, {event}, {block}, 1'001, 1'700'003'000'200);
        });
    EXPECT_TRUE(accepted);
    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 1);

    // Project via the feed-based approach (no longer strand-bound; FeedProjector owns its strand)
    feed::Feed feed_obj(io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);
    EXPECT_EQ(awaitProjectBatch(io_context, store, feed_obj, 8, 1'700'003'000'300), 1u);
    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 1);
}

TEST_F(UnitTest, Events_Concurrency_IngestAndProjectSerializeOnSameHotWriteStrand)
{
    const auto paths = makeTempEventsPaths("concurrency_ingest_project_same_strand");
    asio::io_context io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    auto work_guard = asio::make_work_guard(io_context);
    std::vector<std::thread> io_workers;
    for(int i = 0; i < 4; ++i)
    {
        io_workers.emplace_back([&]
        {
            io_context.run();
        });
    }

    constexpr int EVENT_COUNT = 32;
    std::vector<std::future<bool>> ingest_futures;
    auto hot_write_strand = asio::make_strand(io_context);

    for(int i = 0; i < EVENT_COUNT; ++i)
    {
        auto ingest_promise = std::make_shared<std::promise<bool>>();
        ingest_futures.push_back(ingest_promise->get_future());
        asio::post(hot_write_strand, [&, i, ingest_promise]()
        {
            try
            {
                const std::int64_t block = 3'000 + i;
                const auto event = makeDecodedEvent(
                    block,
                    0,
                    1,
                    static_cast<std::uint8_t>(0x30 + i),
                    static_cast<std::uint8_t>(0x70 + i),
                    events::EventType::TRANSFORMATION_ADDED,
                    events::EventState::OBSERVED,
                    1'700'003'300 + i,
                    1'700'003'300'000 + i);
                const auto block_info = makeBlockInfo(
                    block,
                    event.raw.block_hash,
                    hexBytes(static_cast<std::uint8_t>(0x20 + i), 32),
                    1'700'003'300 + i,
                    1'700'003'300'100 + i);
                const bool ok = store.ingestBatch(
                    "local",
                    CHAIN_ID,
                    std::vector<events::DecodedEvent>{event},
                    std::vector<events::ChainBlockInfo>{block_info},
                    block + 1,
                    1'700'003'300'200 + i);
                ingest_promise->set_value(ok);
            }
            catch(...)
            {
                ingest_promise->set_exception(std::current_exception());
            }
        });
    }

    for(auto & future : ingest_futures)
    {
        EXPECT_TRUE(future.get());
    }

    work_guard.reset();
    io_context.stop();
    for(auto & worker : io_workers)
    {
        worker.join();
    }

    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", EVENT_COUNT);
    events_sql::expectRowCount(paths.hot_db, "normalized_events_hot", EVENT_COUNT);

    // Project via a fresh context so io_context.restart() is safe
    asio::io_context project_io;
    feed::Feed feed_obj(project_io, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);
    const std::size_t total_projected = projectAll(project_io, store, feed_obj, 1'700'003'500'999);
    EXPECT_EQ(total_projected, static_cast<std::size_t>(EVENT_COUNT));
    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", EVENT_COUNT);
}

TEST_F(UnitTest, Events_Runtime_MultipleSources_MergeIntoStorePerSource)
{
    const auto paths = makeTempEventsPaths("runtime_multi_source_merge");

    std::vector<evm::EVM::EmittedLogRecord> chain1_records;
    for(std::uint64_t i = 0; i < 3; ++i)
    {
        chain1_records.push_back(
            makeEmittedRecord(i, 1'000 + static_cast<std::int64_t>(i), static_cast<std::uint8_t>(0x10 + i)));
    }

    std::vector<evm::EVM::EmittedLogRecord> chain2_records;
    for(std::uint64_t i = 0; i < 2; ++i)
    {
        chain2_records.push_back(
            makeEmittedRecord(i, 2'000 + static_cast<std::int64_t>(i), static_cast<std::uint8_t>(0x50 + i)));
    }

    std::vector<std::shared_ptr<events::IEmittedLogSource>> sources;
    sources.push_back(std::make_shared<FakeEmittedLogSource>(1, std::move(chain1_records)));
    sources.push_back(std::make_shared<FakeEmittedLogSource>(2, std::move(chain2_records)));

    asio::io_context io_context;
    events::EventRuntime runtime(
        io_context,
        events::EventRuntimeConfig{
            .hot_db_path = paths.hot_db,
            .chain_id = 1,
            .ingestion_enabled = true,
            .sources = std::move(sources),
            .poll_interval_ms = 20,
            .projector_interval_ms = 20,
            .wal_checkpoint_interval_ms = 5'000
        });

    runtime.start();
    std::thread io_worker([&]
    {
        io_context.run();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto stop_future = asio::co_spawn(io_context, runtime.stop(), asio::use_future);
    stop_future.get();
    io_worker.join();

    // Both sources merged into the one store, partitioned by source_id...
    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM raw_events_hot WHERE source_id='fake:1';"), 3);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM raw_events_hot WHERE source_id='fake:2';"), 2);

    // ...with independent per-source cursors persisted (next_cursor = last seq + 1).
    EXPECT_EQ(db.scalarInt64("SELECT next_cursor FROM source_resume WHERE source_id='fake:1';"), 3);
    EXPECT_EQ(db.scalarInt64("SELECT next_cursor FROM source_resume WHERE source_id='fake:2';"), 2);
}

TEST_F(UnitTest, Events_Concurrency_ReadersDuringWriter_DoNotCorruptState)
{
    const auto paths = makeTempEventsPaths("concurrency_read_write");
    asio::io_context store_io_context;
    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);
    feed::Feed feed_obj(store_io_context, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000, CHAIN_ID);

    std::atomic<bool> done{false};
    std::atomic<int> read_iterations{0};

    std::thread reader([&]
    {
        while(!done.load(std::memory_order_acquire))
        {
            (void)feed_obj.getFeedPage(feed::FeedQuery{
                .limit = 16,
                .include_unfinalized = true
            });
            (void)feed_obj.getStreamPage(feed::StreamQuery{
                .since_seq = 0,
                .limit = 16
            });
            ++read_iterations;
        }
    });

    for(int i = 0; i < 24; ++i)
    {
        const std::int64_t block = 2'000 + i;
        events::DecodedEvent event = makeDecodedEvent(
            block,
            0,
            1,
            static_cast<std::uint8_t>(0x55 + i),
            static_cast<std::uint8_t>(0xA0 + i),
            events::EventType::TRANSFORMATION_ADDED,
            events::EventState::OBSERVED,
            1'700'003'500 + i,
            1'700'003'500'000 + i);
        const events::ChainBlockInfo block_info = makeBlockInfo(
            block,
            event.raw.block_hash,
            hexBytes(0x44, 32),
            1'700'003'500 + i,
            1'700'003'500'100 + i);

        ASSERT_TRUE(awaitIngestBatch(
            store_io_context,
            store,
            CHAIN_ID,
            {event},
            {block_info},
            block + 1,
            1'700'003'500'200 + i));
    }

    EXPECT_EQ(projectAll(store_io_context, store, feed_obj, 1'700'003'501'000), 24u);

    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_GT(read_iterations.load(), 0);
    events_sql::expectRowCount(paths.hot_db, "raw_events_hot", 24);
    events_sql::expectRowCount(paths.feed_db, "feed_items_hot", 24);
    {
        SqliteReadonly fdb(paths.feed_db);
        EXPECT_EQ(fdb.scalarInt64("SELECT COUNT(DISTINCT feed_id) FROM feed_items_hot;"), 24);
    }
}
