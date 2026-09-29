#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "feed_types.hpp"
#include "event_projector.hpp"
#include "sqlite/wal.hpp"

namespace dcn::feed
{
    class IFeedStore
    {
        public:
            virtual ~IFeedStore() = default;
            virtual void applyChange(const events::ChangeRecord & row, std::int64_t now_ms) = 0;
            virtual void maintainOutbox(std::int64_t now_ms) = 0;
            virtual FeedPage getFeedPage(const FeedQuery & query) const = 0;
            virtual StreamPage getStreamPage(const StreamQuery & query) const = 0;
            virtual std::int64_t minAvailableStreamSeq() const = 0;
            virtual std::int64_t getFeedCursor() const = 0;
            virtual void setFeedCursor(std::int64_t last_change_seq) = 0;
            virtual bool runArchiveCycle(std::size_t hot_window_days, std::int64_t now_ms) = 0;
            virtual storage::sqlite::WalCheckpointStats checkpointWal(storage::sqlite::WalCheckpointMode mode) = 0;
    };
}
