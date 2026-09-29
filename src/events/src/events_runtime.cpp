#include <algorithm>
#include <cassert>
#include <chrono>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <spdlog/spdlog.h>
#include <tuple>
#include <variant>
#include <set>
#include <unordered_map>

#include "events.hpp"
#include "async.hpp"
#include "utils.hpp"
#include "parser.hpp"
#include "chain.hpp"
#include "evm.hpp"
#include "local_evm_source.hpp"

namespace dcn::events
{
    EventRuntime::EventRuntime(asio::io_context & io_context, EventRuntimeConfig cfg)
        : _io_context(io_context)
        , _config(std::move(cfg))
        , _write_strand(asio::make_strand(io_context))
        , _store(std::make_shared<SQLiteHotStore>(
            _config.hot_db_path,
            _config.chain_id))
        , _decoder(std::make_unique<PTEventDecoder>())
    {
    }

    EventRuntime::~EventRuntime()
    {
        requestStop();

        if (_active_loop_count.load(std::memory_order_acquire) != 0)
        {
            spdlog::critical("EventRuntime destroyed without awaiting stop()");
            std::terminate();
        }
    }

    void EventRuntime::start()
    {
        if(_running.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        auto spawn_loop = [this](asio::awaitable<void> loop, const char * error_context)
        {
            _active_loop_count.fetch_add(1, std::memory_order_acq_rel);

            asio::co_spawn(
                _io_context,
                [loop = std::move(loop)]() mutable -> asio::awaitable<void>
                {
                    co_await std::move(loop);
                    co_return;
                },
                [this, error_context](std::exception_ptr e)
                {
                    if (e)
                    {
                        utils::logException(e, error_context);
                    }

                    _active_loop_count.fetch_sub(1, std::memory_order_acq_rel);
                });
        };

        spawn_loop(_runProjectorLoop(), "events projector loop failed");
        spawn_loop(_runPruneLoop(), "events prune loop failed");
        spawn_loop(_runMaintenanceLoop(), "events maintenance loop failed");

        if(_config.ingestion_enabled)
        {
            std::set<std::string> seen_source_ids;
            for(const auto & source : _config.sources)
            {
                if(!source)
                {
                    continue;
                }

                const std::string source_id(source->sourceId());
                if(source_id.empty() || !seen_source_ids.insert(source_id).second)
                {
                    spdlog::error(
                        "events ingestion: empty or duplicate source_id='{}' skipped",
                        source_id);
                    continue;
                }

                spawn_loop(_runSourceIngestionLoop(source), "events ingestion loop failed");
            }
        }
    }

    void EventRuntime::requestStop()
    {
        if(_stop_requested.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        _running.store(false, std::memory_order_release);

        // A source waiting on a slow upstream would otherwise hold the stop until its
        // own timeouts expire.
        for(const auto & source : _config.sources)
        {
            if(source) source->cancel();
        }
    }

    asio::awaitable<void> EventRuntime::stop()
    {
        requestStop();
        co_await _waitForLoops();

        try
        {
            (void)co_await checkpointWal(storage::sqlite::WalCheckpointMode::TRUNCATE);
        }
        catch(...)
        {
            utils::logException(std::current_exception(), "events stop checkpoint failed");
        }

        _running.store(false, std::memory_order_release);
        spdlog::info("Events runtime stopped");
        co_return;
    }

    bool EventRuntime::running() const
    {
        return _running.load(std::memory_order_acquire);
    }

    bool EventRuntime::ingestionEnabled() const
    {
        return _config.ingestion_enabled;
    }

    void EventRuntime::addProjector(std::unique_ptr<IEventProjector> projector)
    {
        // Pre-start-only: _projectors is iterated by _runProjectorLoop without synchronization.
        assert(!_running.load(std::memory_order_acquire) && "addProjector must be called before start()");
        _projectors.push_back(std::move(projector));
    }

    SQLiteHotStore & EventRuntime::projectionStore()
    {
        return *_store;
    }

    asio::awaitable<void> EventRuntime::_sleepFor(const std::uint64_t ms) const
    {
        asio::steady_timer timer(co_await asio::this_coro::executor);

        std::uint64_t remaining_ms = ms;
        while(remaining_ms > 0 && !_stop_requested.load(std::memory_order_acquire))
        {
            const std::uint64_t slice_ms = std::min<std::uint64_t>(remaining_ms, 100);
            timer.expires_after(std::chrono::milliseconds(slice_ms));
            std::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if(ec == asio::error::operation_aborted)
            {
                break;
            }
            remaining_ms = (remaining_ms > slice_ms) ? (remaining_ms - slice_ms) : 0;
        }
        co_return;
    }

    asio::awaitable<std::optional<std::uint64_t>> EventRuntime::_storeLoadNextCursor(
        std::string source_id,
        const CursorKind cursor_kind) const
    {
        co_await async::ensureOnStrand(_write_strand);
        co_return _store->loadNextCursor(source_id, cursor_kind);
    }

    asio::awaitable<std::int64_t> EventRuntime::_storeLoadHeadBlock(std::string source_id) const
    {
        co_await async::ensureOnStrand(_write_strand);
        co_return _store->loadHeadBlock(source_id);
    }

    asio::awaitable<FinalityCheckpoint> EventRuntime::_storeLoadFinalityCheckpoint(std::string source_id) const
    {
        co_await async::ensureOnStrand(_write_strand);
        co_return _store->loadFinalityCheckpoint(source_id);
    }

    asio::awaitable<bool> EventRuntime::_storeIngestBatch(
        std::string source_id,
        const CursorKind cursor_kind,
        const std::uint64_t chain_id,
        std::vector<RawChainLog> raw_events,
        std::vector<DecodedEvent> decoded_events,
        std::vector<ChainBlockInfo> block_infos,
        const std::uint64_t next_cursor,
        const std::int64_t now_ms,
        std::optional<ScannedBlockRange> scanned_range) const
    {
        co_await async::ensureOnStrand(_write_strand);
        co_return _store->ingestBatch(
            source_id,
            cursor_kind,
            chain_id,
            raw_events,
            decoded_events,
            block_infos,
            next_cursor,
            now_ms,
            scanned_range);
    }

    asio::awaitable<bool> EventRuntime::_storeApplyFinality(
        std::string source_id,
        const std::uint64_t chain_id,
        FinalityHeights heights,
        const std::int64_t now_ms,
        const std::size_t reorg_window_blocks) const
    {
        co_await async::ensureOnStrand(_write_strand);
        co_return _store->applyFinality(source_id, chain_id, heights, now_ms, reorg_window_blocks);
    }

    asio::awaitable<storage::sqlite::WalCheckpointStats> EventRuntime::checkpointWal(storage::sqlite::WalCheckpointMode mode) const
    {
        co_await async::ensureOnStrand(_write_strand);
        co_return _store->checkpointWal(mode);
    }

    asio::awaitable<void> EventRuntime::_runSourceIngestionLoop(std::shared_ptr<IEmittedLogSource> source)
    {
        const std::string source_id(source->sourceId());
        const std::string chain_namespace(source->chainNamespace());
        const CursorKind cursor_kind = source->cursorKind();
        const std::uint64_t chain_id = source->chainId();
        const bool ephemeral_entities = source->ephemeralEntities();
        const std::uint64_t idle_ms = source->pollInterval()
            ? static_cast<std::uint64_t>(source->pollInterval()->count())
            : _config.poll_interval_ms;

        spdlog::info("Events ingestion loop started for source={} chain={}", source_id, chain_id);
        std::uint64_t next_cursor = (co_await _storeLoadNextCursor(source_id, cursor_kind)).value_or(0);
        source->restoreFinality(co_await _storeLoadFinalityCheckpoint(source_id));
        std::int64_t prior_head = co_await _storeLoadHeadBlock(source_id);

        constexpr std::size_t MAX_POLL_LIMIT = 2048; // ponytail: matches feed::MAX_STREAM_LIMIT
        const std::size_t poll_limit = std::max<std::size_t>(1, std::min<std::size_t>(MAX_POLL_LIMIT, _config.block_batch_size * 8ULL));

        while(!_stop_requested.load(std::memory_order_acquire))
        {
            SourcePoll poll = co_await source->pollSince(next_cursor, poll_limit);

            // The in-process EVM restarted with a shorter history, so its log sequence
            // restarted too: resume from its beginning.
            if(cursor_kind == CursorKind::LOCAL_SEQUENCE && poll.finality && prior_head > poll.finality->head)
            {
                next_cursor = 0;
                if(!co_await _storeIngestBatch(
                    source_id, cursor_kind, chain_id, {}, {}, {}, next_cursor, utils::nowMs(), std::nullopt))
                {
                    co_await _sleepFor(idle_ms);
                }
                prior_head = poll.finality->head;
                continue;
            }

            std::vector<RawChainLog> raw_logs;
            raw_logs.reserve(poll.records.size());

            std::vector<DecodedEvent> decoded_events;
            decoded_events.reserve(poll.records.size());

            std::unordered_map<std::int64_t, ChainBlockInfo> block_map;
            block_map.reserve(poll.records.size());

            const std::int64_t now_ms = utils::nowMs();

            for(const auto & row : poll.records)
            {
                ChainBlockInfo info{
                    .source_id = source_id,
                    .chain_id = chain_id,
                    .block_number = row.block_number,
                    .block_hash = chain::normalizeHex(row.block_hash),
                    .parent_hash = chain::normalizeHex(row.parent_hash),
                    .block_time = row.block_time,
                    .seen_at_ms = now_ms
                };

                block_map.insert_or_assign(info.block_number, std::move(info));

                RawChainLog raw{
                    .source_id = source_id,
                    .chain_namespace = chain_namespace,
                    .chain_id = chain_id,
                    .block_number = row.block_number,
                    .block_hash = chain::normalizeHex(row.block_hash),
                    .parent_hash = chain::normalizeHex(row.parent_hash),
                    .tx_index = row.tx_index,
                    .tx_hash = chain::normalizeHex(row.tx_hash),
                    .log_index = row.log_index,
                    .address = chain::normalizeHex(row.address),
                    .topics = {},
                    .data_hex = chain::normalizeHex(row.data_hex),
                    .removed = false,
                    .block_time = row.block_time,
                    .seen_at_ms = now_ms
                };

                for(std::size_t i = 0; i < std::min<std::size_t>(raw.topics.size(), row.topics.size()); ++i)
                {
                    raw.topics[i] = chain::normalizeHex(row.topics[i]);
                }

                raw_logs.push_back(raw);

                // One malformed log must never take the whole ingestion loop down: it is
                // kept as raw evidence and recorded as a decode failure instead.
                std::optional<DecodedEvent> decoded;
                try
                {
                    decoded = _decoder->decode(raw);
                }
                catch(...)
                {
                    utils::logException(
                        std::current_exception(),
                        std::format("events decode failed for source={} block={} log={}",
                            source_id, raw.block_number, raw.log_index));
                }

                if(!decoded.has_value())
                {
                    continue;
                }

                DecodedEvent event = std::move(*decoded);

                // History the source already reports final goes in final, without the
                // observed -> safe -> finalized churn a large backfill would otherwise cause.
                if(poll.finality && event.state == EventState::OBSERVED
                   && event.raw.block_number <= poll.finality->finalized)
                {
                    event.state = EventState::FINALIZED;
                }

                if(ephemeral_entities)
                {
                    event.entity_address = std::string(EPHEMERAL_ENTITY_ADDRESS);

                    json payload = json::parse(event.decoded_json, nullptr, false);

                    if(!payload.is_discarded() && payload.is_object())
                    {
                        switch(event.event_type)
                        {
                        case EventType::CONNECTOR_ADDED:
                            payload["connector_address"] = std::string(EPHEMERAL_ENTITY_ADDRESS);
                            break;
                        case EventType::TRANSFORMATION_ADDED:
                            payload["transformation_address"] = std::string(EPHEMERAL_ENTITY_ADDRESS);
                            break;
                        case EventType::CONDITION_ADDED:
                            payload["condition_address"] = std::string(EPHEMERAL_ENTITY_ADDRESS);
                            break;
                        case EventType::CONNECTOR_REMOVED:
                        case EventType::TRANSFORMATION_REMOVED:
                        case EventType::CONDITION_REMOVED:
                        case EventType::ENTITY_OWNER_CHANGED:
                            break;
                        }
                        event.decoded_json = payload.dump(-1, ' ', false, json::error_handler_t::replace);
                    }
                }

                decoded_events.push_back(std::move(event));
            }

            std::vector<ChainBlockInfo> block_infos;
            block_infos.reserve(block_map.size());

            for(auto & [_, info] : block_map)
            {
                block_infos.push_back(std::move(info));
            }

            std::ranges::sort(block_infos, [](const ChainBlockInfo & lhs, const ChainBlockInfo & rhs)
            {
                return lhs.block_number < rhs.block_number;
            });

            // An empty range still commits its cursor and still reconciles: a replacement
            // block containing no PT event is exactly how a reorg erases a log without
            // producing one.
            const bool ingest_ok = co_await _storeIngestBatch(
                source_id,
                cursor_kind,
                chain_id,
                std::move(raw_logs),
                std::move(decoded_events),
                std::move(block_infos),
                poll.next_cursor,
                now_ms,
                poll.scanned_range);

            if(!ingest_ok)
            {
                co_await _sleepFor(idle_ms);
                continue;
            }

            source->committed();
            next_cursor = poll.next_cursor;

            if(poll.finality)
            {
                prior_head = std::max(prior_head, poll.finality->head);
                if(!co_await _storeApplyFinality(source_id, chain_id, *poll.finality, now_ms, _config.reorg_window_blocks))
                {
                    spdlog::error("Failed to apply finality during ingestion loop for source={}", source_id);
                }
            }

            if(poll.caught_up)
            {
                co_await _sleepFor(idle_ms);
            }
        }

        co_await source->stop();
        spdlog::info("Events ingestion loop stopped for source={}", source_id);
        co_return;
    }

    asio::awaitable<void> EventRuntime::_runProjectorLoop()
    {
        spdlog::info("Events projector loop started");

        while(!_stop_requested.load(std::memory_order_acquire))
        {
            std::size_t total = 0;
            for(auto & p : _projectors)
            {
                // Isolate per-projector failures: a transient DB error (e.g. SQLITE_BUSY
                // surfaced as an exception) must not kill the loop coroutine for good.
                try
                {
                    total += co_await p->projectBatch(DEFAULT_PROJECT_BATCH_SIZE, utils::nowMs());
                }
                catch(...)
                {
                    utils::logException(std::current_exception(),
                        std::format("events projector '{}' batch failed", p->id()));
                }
            }
            if(total == 0) co_await _sleepFor(_config.projector_interval_ms);
        }

        spdlog::info("Events projector loop stopped");
        co_return;
    }

    asio::awaitable<void> EventRuntime::_runPruneLoop()
    {
        spdlog::info("Events prune loop started");

        while(!_stop_requested.load(std::memory_order_acquire))
        {
            co_await _sleepFor(_config.prune_interval_ms);
            if(_stop_requested.load(std::memory_order_acquire))
            {
                break;
            }

            co_await async::ensureOnStrand(_write_strand);

            if(_projectors.empty())
            {
                continue;
            }

            // watermark = min cursor over all projectors (skip if none have advanced)
            std::int64_t watermark = std::numeric_limits<std::int64_t>::max();
            for(const auto & p : _projectors)
            {
                watermark = std::min(watermark, p->cursor());
            }

            if(watermark <= 0)
            {
                continue;
            }

            constexpr std::size_t PRUNE_BATCH = 500;
            for(const auto & source : _config.sources)
            {
                if(!source) continue;
                const std::string source_id(source->sourceId());
                const std::int64_t head = _store->loadHeadBlock(source_id);
                if(head <= 0) continue;

                const std::int64_t floor =
                    (head > static_cast<std::int64_t>(_config.reorg_window_blocks))
                        ? (head - static_cast<std::int64_t>(_config.reorg_window_blocks))
                        : 0;

                (void)_store->pruneConsumedRaw(source_id, watermark, floor, PRUNE_BATCH);
            }
        }

        spdlog::info("Events prune loop stopped");
        co_return;
    }

    asio::awaitable<void> EventRuntime::_runMaintenanceLoop()
    {
        spdlog::info("Events maintenance loop started");

        while(!_stop_requested.load(std::memory_order_acquire))
        {
            co_await _sleepFor(_config.wal_checkpoint_interval_ms);
            if(_stop_requested.load(std::memory_order_acquire))
            {
                break;
            }

            (void)co_await checkpointWal(storage::sqlite::WalCheckpointMode::PASSIVE);
        }

        spdlog::info("Events maintenance loop stopped");
        co_return;
    }

    asio::awaitable<void> EventRuntime::_waitForLoops()
    {
        asio::steady_timer timer(co_await asio::this_coro::executor);

        while(_active_loop_count.load(std::memory_order_acquire) > 0)
        {
            timer.expires_after(std::chrono::milliseconds(5));
            std::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if(ec == asio::error::operation_aborted)
            {
                continue;
            }
        }
        co_return;
    }

}
