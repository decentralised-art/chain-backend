#include <spdlog/spdlog.h>

#include "feed_projector.hpp"
#include "async.hpp"

namespace dcn::feed
{
    FeedProjector::FeedProjector(
        events::SQLiteHotStore & store,
        Feed & feed,
        const asio::strand<asio::io_context::executor_type> & write_strand,
        events::ProjectorRetryConfig retry)
        : _store(store)
        , _feed(feed)
        , _write_strand(write_strand)
        , _retry(retry)
        , _last_change_seq(feed.getFeedCursor())
    {
    }

    std::string_view FeedProjector::id() const
    {
        return events::FEED_PROJECTOR_ID;
    }

    asio::awaitable<std::size_t> FeedProjector::projectBatch(std::size_t limit, std::int64_t now_ms)
    {
        co_await async::ensureOnStrand(_write_strand);
        auto rows = _store.readChangesSince(_last_change_seq, limit);

        std::int64_t last_consumed_seq = _last_change_seq;
        std::size_t processed_count = 0;
        for(const auto & row : rows)
        {
            // co_await is not allowed inside a catch handler, so the failure path
            // runs after the try/catch on an `applied` flag.
            bool applied = false;
            try
            {
                co_await _feed.applyChange(row, now_ms);
                applied = true;
            }
            catch(const std::exception & e)
            {
                spdlog::error("FeedProjector::projectBatch: applyChange failed at change_seq={}: {}",
                              row.change_seq, e.what());
            }

            if(!applied)
            {
                if(row.change_seq == _failing_seq)
                {
                    ++_failure_count;
                }
                else
                {
                    _failing_seq = row.change_seq;
                    _failure_count = 1;
                }

                if(_failure_count < _retry.max_attempts)
                {
                    // Leave the cursor before this row so it is retried next pass;
                    // transient failures (e.g. busy feed DB) heal across passes.
                    break;
                }

                // Retry budget exhausted: dead-letter the row and advance past it rather
                // than wedge the pipeline (a permanently parked cursor freezes the prune
                // watermark for every projector, growing the hot store without bound).
                // The dead-letter bit exempts the row from pruning, so the raw chain
                // evidence is retained and the idle-time sweep keeps retrying it — no
                // chain event data is lost.
                co_await async::ensureOnStrand(_write_strand);
                if(!_store.markDeadLetter(
                    events::FEED_DEAD_LETTER_BIT, row.source_id, row.block_hash, row.log_index))
                {
                    // The quarantine could not be made durable; advancing now would let
                    // pruning delete the row, so keep the cursor parked and retry.
                    spdlog::error("FeedProjector: failed to dead-letter change_seq={} — "
                        "cursor stays parked", row.change_seq);
                    break;
                }

                spdlog::error("FeedProjector: dead-lettered change_seq={} "
                    "(block={} log_index={} event_type='{}') after {} failed attempts; "
                    "row is retained in the hot store and retried by the sweep",
                    row.change_seq, row.block_number, row.log_index, row.event_type,
                    _failure_count);
                _failing_seq = 0;
                _failure_count = 0;
            }

            last_consumed_seq = row.change_seq;
            ++processed_count;
        }
        if(last_consumed_seq > _last_change_seq)
        {
            co_await _feed.setFeedCursor(last_consumed_seq);
            _last_change_seq = last_consumed_seq;
        }
        co_await _feed.maintainOutbox(now_ms);

        // Retry dead-lettered rows only when the main changelog is drained, so the
        // sweep never competes with live projection.
        if(rows.empty())
        {
            processed_count += co_await _sweepDeadLetters(limit, now_ms);
        }

        co_return processed_count;
    }

    asio::awaitable<std::size_t> FeedProjector::_sweepDeadLetters(std::size_t limit, std::int64_t now_ms)
    {
        if(now_ms - _last_sweep_ms < _retry.sweep_interval_ms)
        {
            co_return 0;
        }
        _last_sweep_ms = now_ms;

        co_await async::ensureOnStrand(_write_strand);
        const auto rows = _store.readDeadLetters(events::FEED_DEAD_LETTER_BIT, limit);

        std::size_t resolved = 0;
        for(const auto & row : rows)
        {
            bool applied = false;
            try
            {
                co_await _feed.applyChange(row, now_ms);
                applied = true;
            }
            catch(const std::exception & e)
            {
                spdlog::error("FeedProjector sweep: applyChange failed at change_seq={}: {}",
                              row.change_seq, e.what());
            }

            if(!applied)
            {
                // Still failing; the bit stays set, the row stays retained, and the
                // next sweep retries it.
                continue;
            }

            co_await async::ensureOnStrand(_write_strand);
            if(_store.clearDeadLetter(
                events::FEED_DEAD_LETTER_BIT, row.source_id, row.block_hash, row.log_index))
            {
                spdlog::info("FeedProjector: dead-lettered change_seq={} resolved "
                    "(block={} log_index={} event_type='{}')",
                    row.change_seq, row.block_number, row.log_index, row.event_type);
                ++resolved;
            }
        }

        co_return resolved;
    }
}
