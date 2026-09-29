#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#ifndef DECENTRALISED_ART_TEST_SOLC_PATH
    #error "DECENTRALISED_ART_TEST_SOLC_PATH is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_PT_PATH
    #error "DECENTRALISED_ART_TEST_PT_PATH is not defined"
#endif

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    std::filesystem::path solcPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_SOLC_PATH);
    }

    std::filesystem::path ptPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_PT_PATH);
    }

    const std::string TRANSFORMATION_ADDED_SIG = "TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)";

    evm::EVM::EmittedLogRecord makeRecord(
        const std::uint64_t seq,
        const std::int64_t block_number,
        const std::uint8_t entity_byte)
    {
        evm::EVM::EmittedLogRecord record{};
        record.seq = seq;
        record.block_number = block_number;
        record.block_hash = hexBytes(static_cast<std::uint8_t>(block_number & 0xFF), 32);
        record.parent_hash = hexBytes(0x7F, 32);
        record.tx_index = 0;
        record.tx_hash = hexBytes(0x22, 32);
        record.log_index = 0;
        record.address = hexAddress(0xAB);
        record.topics = { topicForEvent(TRANSFORMATION_ADDED_SIG) };
        record.data_hex = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x41),
            "adapter_tx",
            makeAddressFromByte(entity_byte),
            makeAddressFromByte(0x42),
            1);
        record.block_time = 1'700'000'000;
        return record;
    }

    // Programmable EmittedLogRecord source: replays a fixed batch (seq >= cursor),
    // reports a configurable finality head, and records how it was driven so tests
    // can assert ingestion's cursor behaviour (poll count, minimum cursor polled).
    class FakeSource final : public events::IEmittedLogSource
    {
        public:
            FakeSource(std::uint64_t chain_id, std::vector<evm::EVM::EmittedLogRecord> records, bool ephemeral, std::int64_t head)
                : _chain_id(chain_id)
                , _records(std::move(records))
                , _ephemeral(ephemeral)
                , _head(head)
            {
            }

            std::string_view sourceId() const override { return events::LOCAL_SOURCE_ID; }
            std::uint64_t chainId() const override { return _chain_id; }
            std::string_view chainNamespace() const override { return events::LOCAL_CHAIN_NAMESPACE; }
            events::CursorKind cursorKind() const override { return events::CursorKind::LOCAL_SEQUENCE; }
            bool ephemeralEntities() const override { return _ephemeral; }

            asio::awaitable<events::SourcePoll> pollSince(std::uint64_t cursor, std::size_t limit) override
            {
                _poll_count.fetch_add(1, std::memory_order_acq_rel);
                std::uint64_t prev_min = _min_cursor.load(std::memory_order_acquire);
                while(cursor < prev_min && !_min_cursor.compare_exchange_weak(prev_min, cursor)) { }

                std::vector<evm::EVM::EmittedLogRecord> out;
                for(const auto & record : _records)
                {
                    if(record.seq >= cursor && out.size() < limit)
                    {
                        out.push_back(record);
                    }
                }
                const std::uint64_t next_cursor = out.empty() ? cursor : out.back().seq + 1;
                co_return events::SourcePoll{
                    std::move(out),
                    events::FinalityHeights{ .head = _head, .safe = _head, .finalized = _head },
                    next_cursor
                };
            }

            std::uint64_t pollCount() const { return _poll_count.load(std::memory_order_acquire); }
            std::uint64_t minCursorPolled() const { return _min_cursor.load(std::memory_order_acquire); }

        private:
            std::uint64_t _chain_id;
            std::vector<evm::EVM::EmittedLogRecord> _records;
            bool _ephemeral;
            std::int64_t _head;
            std::atomic<std::uint64_t> _poll_count{0};
            std::atomic<std::uint64_t> _min_cursor{std::numeric_limits<std::uint64_t>::max()};
    };

    events::EventRuntimeConfig makeRuntimeConfig(
        const TempEventsPaths & paths,
        std::vector<std::shared_ptr<events::IEmittedLogSource>> sources)
    {
        return events::EventRuntimeConfig{
            .hot_db_path = paths.hot_db,
            .chain_id = CHAIN_ID,
            .ingestion_enabled = true,
            .sources = std::move(sources),
            .poll_interval_ms = 20,
            .projector_interval_ms = 20,
            .wal_checkpoint_interval_ms = 5'000
        };
    }

    void runRuntimeFor(events::EventRuntime & runtime, asio::io_context & io_context, std::chrono::milliseconds duration)
    {
        runtime.start();
        std::thread io_worker([&]{ io_context.run(); });
        std::this_thread::sleep_for(duration);
        asio::co_spawn(io_context, runtime.stop(), asio::use_future).get();
        io_worker.join();
    }
}

// LocalEvmSource adapts the in-process EVM: a real deploy emits a TransformationAdded
// log, and pollSince must surface it with the correct resume cursor, immediate
// finality, and the ephemeral-entity policy flag set.
TEST_F(UnitTest, Events_LocalEvmSource_SurfacesEmittedLogsWithCursorAndFinality)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const auto paths = makeTempEventsPaths("local_evm_source_adapter");
    const auto storage_path = paths.root / "storage";
    std::error_code ec;
    std::filesystem::create_directories(storage_path / "transformations" / "build", ec);
    ASSERT_FALSE(ec) << ec.message();

    asio::io_context io_context;
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    registry::Registry registry(io_context);

    const chain::Address owner = makeAddressFromByte(0x71);
    const std::string owner_hex = evmc::hex(owner);
    (void)runAwaitable(io_context, evm_instance.addAccount(owner, evm::DEFAULT_GAS_LIMIT));
    (void)runAwaitable(io_context, evm_instance.setGas(owner, evm::DEFAULT_GAS_LIMIT));

    TransformationRecord transformation;
    transformation.set_owner(owner_hex);
    transformation.mutable_transformation()->set_name("AdapterTransform");
    transformation.mutable_transformation()->set_sol_src("return x;");

    const auto deploy_result = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation, storage_path));
    ASSERT_TRUE(deploy_result) << std::format("deployTransformation failed: {}", deploy_result.error().kind);

    events::LocalEvmSource source(evm_instance, CHAIN_ID);
    EXPECT_EQ(source.chainId(), CHAIN_ID);
    EXPECT_TRUE(source.ephemeralEntities());

    const auto poll = runAwaitable(io_context, source.pollSince(0, 1024));
    ASSERT_FALSE(poll.records.empty()) << "deploy emitted no logs for the source to surface";

    std::int64_t max_block = 0;
    for(const auto & record : poll.records)
    {
        max_block = std::max(max_block, record.block_number);
    }

    EXPECT_EQ(poll.next_cursor, poll.records.back().seq);
    ASSERT_TRUE(poll.finality.has_value());
    EXPECT_EQ(poll.finality->head, poll.finality->safe);
    EXPECT_EQ(poll.finality->safe, poll.finality->finalized);
    EXPECT_GE(poll.finality->head, max_block);

    // Draining past the end yields nothing and leaves the cursor where it was.
    const auto drained = runAwaitable(io_context, source.pollSince(poll.next_cursor, 1024));
    EXPECT_TRUE(drained.records.empty());
    EXPECT_EQ(drained.next_cursor, poll.next_cursor);

    // The first log emitted after the source caught up is delivered, not skipped: a
    // publication emits exactly one registry event.
    transformation.mutable_transformation()->set_name("LaterTransform");
    const auto later_deploy = runAwaitable(
        io_context,
        loader::deployTransformation(evm_instance, registry, transformation, storage_path));
    ASSERT_TRUE(later_deploy) << std::format("deployTransformation failed: {}", later_deploy.error().kind);

    const auto later = runAwaitable(io_context, source.pollSince(drained.next_cursor, 1024));
    ASSERT_EQ(later.records.size(), 1u);
    EXPECT_EQ(later.records.front().seq, poll.records.back().seq + 1);
    EXPECT_EQ(later.next_cursor, later.records.front().seq);
}

// A source flagged ephemeralEntities() must have its decoded entity addresses
// rewritten to the ephemeral sentinel ("0x0") by ingestion.
TEST_F(UnitTest, Events_Ingestion_EphemeralSource_RewritesEntityAddress)
{
    const auto paths = makeTempEventsPaths("ingestion_ephemeral_rewrite");

    auto source = std::make_shared<FakeSource>(
        CHAIN_ID,
        std::vector<evm::EVM::EmittedLogRecord>{ makeRecord(0, 1'000, 0x43) },
        /*ephemeral=*/true,
        /*head=*/1'000);

    asio::io_context io_context;
    events::EventRuntime runtime(io_context, makeRuntimeConfig(paths, { source }));
    runRuntimeFor(runtime, io_context, std::chrono::milliseconds(200));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM normalized_events_hot;"), 1);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM normalized_events_hot WHERE entity_address='0x0';"), 1);
}

// The store keys cursor state by source_id, so two sources sharing a source_id would
// clobber each other. start() must reject the duplicate: only the first source runs.
TEST_F(UnitTest, Events_Ingestion_DuplicateSourceIdSource_IsRejected)
{
    const auto paths = makeTempEventsPaths("ingestion_duplicate_source_id");

    auto source_a = std::make_shared<FakeSource>(
        CHAIN_ID,
        std::vector<evm::EVM::EmittedLogRecord>{
            makeRecord(0, 1'000, 0x43),
            makeRecord(1, 1'001, 0x44),
            makeRecord(2, 1'002, 0x45) },
        /*ephemeral=*/false,
        /*head=*/1'002);

    auto source_b = std::make_shared<FakeSource>(
        CHAIN_ID,
        std::vector<evm::EVM::EmittedLogRecord>{ makeRecord(0, 2'000, 0x46) },
        /*ephemeral=*/false,
        /*head=*/2'000);

    asio::io_context io_context;
    events::EventRuntime runtime(
        io_context,
        makeRuntimeConfig(paths, { source_a, source_b }));
    runRuntimeFor(runtime, io_context, std::chrono::milliseconds(200));

    // The duplicate source's loop never starts, so it is never polled...
    EXPECT_EQ(source_b->pollCount(), 0u);

    // ...and only the first source's records reach the store.
    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM raw_events_hot WHERE chain_id=1;"), 3);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM raw_events_hot;"), 3);
}

// When a source's height regresses below the persisted cursor (e.g. a local EVM
// restart), ingestion must rewind its local seq cursor to 0 and re-read, rather
// than getting stuck skipping the re-emitted logs.
TEST_F(UnitTest, Events_Ingestion_SourceRegression_RewindsLocalCursor)
{
    const auto paths = makeTempEventsPaths("ingestion_source_regression");

    // Session 1: ingest at high blocks; cursor advances to seq 3 at head 1002.
    std::vector<evm::EVM::EmittedLogRecord> high_records{
        makeRecord(0, 1'000, 0x43),
        makeRecord(1, 1'001, 0x44),
        makeRecord(2, 1'002, 0x45) };
    {
        auto source = std::make_shared<FakeSource>(CHAIN_ID, high_records, /*ephemeral=*/false, /*head=*/1'002);
        asio::io_context io_context;
        events::EventRuntime runtime(io_context, makeRuntimeConfig(paths, { source }));
        runRuntimeFor(runtime, io_context, std::chrono::milliseconds(200));

        SqliteReadonly db(paths.hot_db);
        EXPECT_EQ(db.scalarInt64("SELECT next_cursor FROM source_resume WHERE source_id='local';"), 3);
    }

    // Session 2 (same db): the source restarted and re-emits from low blocks with a
    // regressed head (< the persisted finality head). Ingestion must rewind its local
    // seq cursor to 0, re-read the restarted logs, then settle back at seq 3.
    std::vector<evm::EVM::EmittedLogRecord> low_records{
        makeRecord(0, 1, 0x43),
        makeRecord(1, 2, 0x44),
        makeRecord(2, 3, 0x45) };
    {
        auto source = std::make_shared<FakeSource>(CHAIN_ID, low_records, /*ephemeral=*/false, /*head=*/10);
        asio::io_context io_context;
        events::EventRuntime runtime(io_context, makeRuntimeConfig(paths, { source }));
        runRuntimeFor(runtime, io_context, std::chrono::milliseconds(200));

        // Without the rewind the loop would only ever poll from the persisted cursor (3).
        EXPECT_EQ(source->minCursorPolled(), 0u);

        SqliteReadonly db(paths.hot_db);
        EXPECT_EQ(db.scalarInt64("SELECT next_cursor FROM source_resume WHERE source_id='local';"), 3);
    }
}
