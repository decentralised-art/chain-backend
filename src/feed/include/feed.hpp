#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "async.hpp"
#include "feed_types.hpp"
#include "feed_store.hpp"
#include "sqlite_feed_store.hpp"
#include "event_projector.hpp"
#include "sqlite/wal_store.hpp"

namespace dcn::feed
{
    class Feed : public IFeedRepository, public storage::sqlite::IWalStore
    {
        public:
            Feed(asio::io_context & io_context,
                 std::filesystem::path feed_db_path,
                 std::filesystem::path archive_root,
                 std::int64_t outbox_retention_ms);

            Feed(const Feed&) = delete;
            Feed& operator=(const Feed&) = delete;

            // write side (serialize on _strand)
            asio::awaitable<void> applyChange(events::ChangeRecord row, std::int64_t now_ms);
            asio::awaitable<void> maintainOutbox(std::int64_t now_ms);
            // Exports every chain's finalized rows into monthly shards, then prunes exported
            // rows older than the hot window.
            asio::awaitable<bool> runArchiveCycle(std::size_t hot_window_days, std::int64_t now_ms);

            // query side (IFeedRepository; delegates to _store, callers own concurrency)
            FeedPage getFeedPage(const FeedQuery & query) const override;
            StreamPage getStreamPage(const StreamQuery & query) const override;
            std::int64_t minAvailableStreamSeq() const override;

            // cursor
            // getFeedCursor is synchronous: read once at construction (pre-start, single-threaded)
            std::int64_t getFeedCursor() const;
            // setFeedCursor runs on the _strand so cursor writes serialize with all other store access
            asio::awaitable<void> setFeedCursor(std::int64_t seq);

            asio::awaitable<storage::sqlite::WalCheckpointStats> checkpointWal(
                storage::sqlite::WalCheckpointMode mode) const override;

        private:
            asio::strand<asio::io_context::executor_type> _strand;
            std::unique_ptr<IFeedStore> _store;
    };
}
