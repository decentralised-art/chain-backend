#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "entity.hpp"
#include "native.h"
#include <asio.hpp>

namespace dcn::events
{
    constexpr std::string_view FEED_PROJECTOR_ID = "feed";
    constexpr std::string_view REGISTRY_PROJECTOR_ID = "registry";

    // Bits in normalized_events_hot.dead_letter. A projector sets its bit when it
    // exhausts its retry budget on a row and advances its cursor past it. Rows with
    // any bit set are exempt from pruning, so the raw chain evidence is retained
    // until every marking projector resolves the row and clears its bit.
    constexpr int FEED_DEAD_LETTER_BIT = 1 << 0;
    constexpr int REGISTRY_DEAD_LETTER_BIT = 1 << 1;

    // Retry/quarantine tuning shared by the projectors.
    struct ProjectorRetryConfig
    {
        // Consecutive failing passes on the same row before it is dead-lettered and
        // the cursor advances past it. Values <= 1 dead-letter on the first failure.
        std::size_t max_attempts = 5;

        // Minimum time between idle-time dead-letter retry sweeps; dead letters are
        // rare, so a slow cadence keeps the recovery path cheap.
        std::int64_t sweep_interval_ms = 60'000;
    };

    struct ChangeRecord
    {
        std::string source_id = "local";
        std::string chain_namespace = "local";
        std::string registry_address = "0x0";
        std::uint64_t chain_id = 0;
        std::string block_hash;
        std::int64_t log_index = 0;
        std::int64_t change_seq = 0;
        std::string event_type;
        std::string state;
        std::string name;
        std::string owner;
        std::string tx_hash;
        std::optional<std::int64_t> block_time;
        std::string data_hex;
        std::array<std::optional<std::string>, 4> topics;
        std::int64_t block_number = 0;
        std::int64_t tx_index = 0;
        std::optional<std::string> content_hash;
        std::optional<std::string> metadata_hash;
        std::optional<std::string> runtime_code_hash;
        std::optional<pt::EntityKind> entity_kind;
        std::optional<std::string> old_owner;
        std::string entity_address;
    };

    class IEventProjector
    {
        public:
            virtual ~IEventProjector() = default;
            virtual std::string_view id() const = 0;
            virtual std::int64_t cursor() const = 0;
            virtual asio::awaitable<std::size_t> projectBatch(std::size_t limit, std::int64_t now_ms) = 0;
    };
}
