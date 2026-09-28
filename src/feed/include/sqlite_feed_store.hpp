#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>

#include "events_shard.hpp"
#include "feed_store.hpp"

namespace dcn::feed
{
    class SqliteFeedStore final : public IFeedStore
    {
        public:
            SqliteFeedStore(
                const std::filesystem::path & feed_db_path,
                const std::filesystem::path & archive_root,
                std::int64_t outbox_retention_ms,
                std::uint64_t default_chain_id);

            ~SqliteFeedStore() override;

            void applyChange(const events::ChangeRecord & row, std::int64_t now_ms) override;
            void maintainOutbox(std::int64_t now_ms) override;
            FeedPage getFeedPage(const FeedQuery & query) const override;
            StreamPage getStreamPage(const StreamQuery & query) const override;
            std::int64_t minAvailableStreamSeq() const override;
            std::int64_t getFeedCursor() const override;
            void setFeedCursor(std::int64_t last_change_seq) override;
            bool runArchiveCycle(std::uint64_t chain_id, std::size_t hot_window_days, std::int64_t now_ms) override;
            storage::sqlite::WalCheckpointStats checkpointWal(storage::sqlite::WalCheckpointMode mode) override;

        private:
            bool _initializeFeedSchema();
            bool _initializeArchiveSchema(sqlite3 * archive_db) const;
            bool _exportMonth(const std::uint64_t chain_id, const std::string & month_token, const std::int64_t now_ms);
            std::vector<std::filesystem::path> _candidateArchivePaths(const std::optional<CursorKey> & before_key) const;
            void _appendFeedRowsFromDatabase(
                sqlite3 * db,
                const char * table_name,
                const FeedQuery & query,
                const std::optional<CursorKey> & before_key,
                const std::size_t limit,
                std::vector<FeedItem> & out_items,
                std::unordered_set<std::string> & seen_feed_ids) const;

        private:
            std::filesystem::path _feed_db_path;
            std::filesystem::path _archive_root;
            std::int64_t _outbox_retention_ms = 0;

            sqlite3 * _write_db = nullptr;
            sqlite3 * _read_db = nullptr;
            std::unique_ptr<events::IEventShardRouter> _shard_router;

            std::uint64_t _default_chain_id = 1;
    };
}
