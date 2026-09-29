#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "native.h"
#include <asio.hpp>

#include "feed.hpp"

namespace dcn::feed
{
    struct FeedRuntimeConfig
    {
        std::filesystem::path feed_db_path;
        std::filesystem::path archive_root;
        std::size_t hot_window_days = 90;
        std::int64_t outbox_retention_ms = 7LL * 24 * 60 * 60 * 1000;
        unsigned int archive_interval_ms = 30 * 1000;
        unsigned int wal_checkpoint_interval_ms = 15 * 1000;
    };

    class FeedRuntime final
    {
        public:
            FeedRuntime(asio::io_context & io_context, FeedRuntimeConfig config);
            ~FeedRuntime();

            FeedRuntime(const FeedRuntime &) = delete;
            FeedRuntime & operator=(const FeedRuntime &) = delete;

            FeedRuntime(FeedRuntime &&) = delete;
            FeedRuntime & operator=(FeedRuntime &&) = delete;

            void start();
            void requestStop();
            asio::awaitable<void> stop();

            Feed & feed();

        private:
            asio::awaitable<void> _runArchiveLoop();
            asio::awaitable<void> _runWalCheckpointLoop();
            asio::awaitable<void> _sleepFor(std::uint64_t ms) const;
            asio::awaitable<void> _waitForLoops();

            asio::io_context & _io_context;
            FeedRuntimeConfig _config;
            Feed _feed;

            std::atomic<bool> _stop_requested{false};
            std::atomic<bool> _running{false};
            std::atomic<std::size_t> _active_loop_count{0};
    };
}
