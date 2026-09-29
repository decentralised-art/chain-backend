#include <chrono>
#include <exception>
#include <system_error>

#include <spdlog/spdlog.h>

#include "feed_runtime.hpp"
#include "utils.hpp"

namespace dcn::feed
{
    FeedRuntime::FeedRuntime(asio::io_context & io_context, FeedRuntimeConfig config)
        : _io_context(io_context)
        , _config(std::move(config))
        , _feed(io_context,
                _config.feed_db_path,
                _config.archive_root,
                _config.outbox_retention_ms)
    {
    }

    FeedRuntime::~FeedRuntime()
    {
        requestStop();

        if(_active_loop_count.load(std::memory_order_acquire) != 0)
        {
            spdlog::critical("FeedRuntime destroyed without awaiting stop()");
            std::terminate();
        }
    }

    void FeedRuntime::start()
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
                    if(e)
                    {
                        utils::logException(e, error_context);
                    }

                    _active_loop_count.fetch_sub(1, std::memory_order_acq_rel);
                });
        };

        spawn_loop(_runArchiveLoop(), "feed archive loop failed");
        spawn_loop(_runWalCheckpointLoop(), "feed WAL checkpoint loop failed");
    }

    void FeedRuntime::requestStop()
    {
        if(_stop_requested.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        _running.store(false, std::memory_order_release);
    }

    asio::awaitable<void> FeedRuntime::stop()
    {
        requestStop();
        co_await _waitForLoops();

        try
        {
            (void)co_await _feed.checkpointWal(storage::sqlite::WalCheckpointMode::TRUNCATE);
        }
        catch(...)
        {
            utils::logException(std::current_exception(), "feed stop checkpoint failed");
        }

        _running.store(false, std::memory_order_release);
        spdlog::info("Feed runtime stopped");
        co_return;
    }

    Feed & FeedRuntime::feed()
    {
        return _feed;
    }

    asio::awaitable<void> FeedRuntime::_runArchiveLoop()
    {
        spdlog::info("Feed archive loop started");

        while(!_stop_requested.load(std::memory_order_acquire))
        {
            co_await _sleepFor(_config.archive_interval_ms);
            if(_stop_requested.load(std::memory_order_acquire))
            {
                break;
            }

            (void)co_await _feed.runArchiveCycle(_config.hot_window_days, utils::nowMs());
        }

        spdlog::info("Feed archive loop stopped");
        co_return;
    }

    asio::awaitable<void> FeedRuntime::_runWalCheckpointLoop()
    {
        spdlog::info("Feed WAL checkpoint loop started");

        while(!_stop_requested.load(std::memory_order_acquire))
        {
            co_await _sleepFor(_config.wal_checkpoint_interval_ms);
            if(_stop_requested.load(std::memory_order_acquire))
            {
                break;
            }

            (void)co_await _feed.checkpointWal(storage::sqlite::WalCheckpointMode::PASSIVE);
        }

        spdlog::info("Feed WAL checkpoint loop stopped");
        co_return;
    }

    asio::awaitable<void> FeedRuntime::_sleepFor(const std::uint64_t ms) const
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

    asio::awaitable<void> FeedRuntime::_waitForLoops()
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
