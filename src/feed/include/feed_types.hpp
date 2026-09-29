#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <format>

#include <nlohmann/json.hpp>

#include "parser.hpp"

namespace dcn::feed
{
    constexpr std::size_t DEFAULT_FEED_LIMIT = 100;
    constexpr std::size_t MAX_FEED_LIMIT = 256;
    constexpr std::size_t DEFAULT_STREAM_LIMIT = 500;
    constexpr std::size_t MAX_STREAM_LIMIT = 2048;

    struct CursorKey
    {
        std::uint64_t chain_id = 1;
        std::string chain_namespace = "local";
        std::int64_t created_at_ms = 0;
        std::int64_t block_number = 0;
        std::int64_t tx_index = 0;
        std::string feed_id;
    };

    struct FeedItem
    {
        std::string feed_id;
        std::string event_type;
        std::string status;
        bool visible = true;
        std::string tx_hash;
        std::int64_t block_number = 0;
        std::int64_t tx_index = 0;
        std::int64_t log_index = 0;
        std::string history_cursor;
        std::int64_t created_at_ms = 0;
        std::int64_t updated_at_ms = 0;
        int projector_version = 1;
        nlohmann::json payload;
    };

    struct FeedQuery
    {
        std::size_t limit = DEFAULT_FEED_LIMIT;
        std::optional<std::string> before_cursor = std::nullopt;
        std::optional<std::string> event_type = std::nullopt;
        bool include_unfinalized = true;
    };

    struct FeedPage
    {
        std::vector<FeedItem> items;
        std::optional<std::string> next_before_cursor = std::nullopt;
        bool has_more = false;
    };

    struct StreamQuery
    {
        std::int64_t since_seq = 0;
        std::size_t limit = 200;
    };

    struct StreamDelta
    {
        std::int64_t stream_seq = 0;
        std::string event_type;
        std::string status;
        std::string feed_id;
        std::string history_cursor;
        std::int64_t created_at_ms = 0;
        nlohmann::json payload;
    };

    struct StreamPage
    {
        std::vector<StreamDelta> deltas;
        std::optional<std::int64_t> last_seq = std::nullopt;
        std::int64_t min_available_seq = 0;
        std::int64_t replay_floor_seq = 0;
        bool stale_since_seq = false;
        bool has_more = false;
    };

    class IFeedRepository
    {
        public:
            virtual ~IFeedRepository() = default;

            virtual FeedPage getFeedPage(const FeedQuery & query) const = 0;
            virtual StreamPage getStreamPage(const StreamQuery & query) const = 0;
            virtual std::int64_t minAvailableStreamSeq() const = 0;
    };

    parse::Result<CursorKey> parseHistoryCursor(const std::string & cursor);
}

template <>
struct std::formatter<dcn::feed::CursorKey> : std::formatter<std::string> {
    auto format(const dcn::feed::CursorKey & cursor, format_context& ctx) const {
        return std::formatter<std::string>::format(
            std::format("c{}:{}:{}:{}",
                cursor.created_at_ms,
                cursor.block_number,
                cursor.tx_index,
                cursor.feed_id),
        ctx);
    }
};
