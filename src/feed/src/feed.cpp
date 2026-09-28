#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "feed.hpp"
#include "sqlite_feed_store.hpp"

namespace dcn::feed
{
    Feed::Feed(
        asio::io_context & io_context,
        std::filesystem::path feed_db_path,
        std::filesystem::path archive_root,
        std::int64_t outbox_retention_ms,
        std::uint64_t default_chain_id)
        : _strand(asio::make_strand(io_context))
        , _store(std::make_unique<SqliteFeedStore>(
            std::move(feed_db_path),
            std::move(archive_root),
            outbox_retention_ms,
            default_chain_id))
    {
    }

    asio::awaitable<void> Feed::applyChange(events::ChangeRecord row, std::int64_t now_ms)
    {
        co_await async::ensureOnStrand(_strand);
        _store->applyChange(row, now_ms);
    }

    asio::awaitable<void> Feed::maintainOutbox(std::int64_t now_ms)
    {
        co_await async::ensureOnStrand(_strand);
        _store->maintainOutbox(now_ms);
    }

    asio::awaitable<bool> Feed::runArchiveCycle(std::uint64_t chain_id, std::size_t hot_window_days, std::int64_t now_ms)
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->runArchiveCycle(chain_id, hot_window_days, now_ms);
    }

    FeedPage Feed::getFeedPage(const FeedQuery & query) const
    {
        return _store->getFeedPage(query);
    }

    StreamPage Feed::getStreamPage(const StreamQuery & query) const
    {
        return _store->getStreamPage(query);
    }

    std::int64_t Feed::minAvailableStreamSeq() const
    {
        return _store->minAvailableStreamSeq();
    }

    std::int64_t Feed::getFeedCursor() const
    {
        return _store->getFeedCursor();
    }

    asio::awaitable<void> Feed::setFeedCursor(std::int64_t seq)
    {
        co_await async::ensureOnStrand(_strand);
        _store->setFeedCursor(seq);
    }

    asio::awaitable<storage::sqlite::WalCheckpointStats> Feed::checkpointWal(
        storage::sqlite::WalCheckpointMode mode) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->checkpointWal(mode);
    }
}
