#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "entity.hpp"
#include "parser.hpp"

namespace dcn::events
{
    constexpr std::string_view LOCAL_SOURCE_ID = "local";
    constexpr std::string_view LOCAL_CHAIN_NAMESPACE = "local";

    // Address recorded for entities of a source with ephemeral entities: the local EVM
    // redeploys them at will, so their addresses carry no meaning outside the process.
    constexpr std::string_view EPHEMERAL_ENTITY_ADDRESS = "0x0";

    constexpr std::string_view CONNECTOR_ADDED_TYPE = "connector_added";
    constexpr std::string_view TRANSFORMATION_ADDED_TYPE = "transformation_added";
    constexpr std::string_view CONDITION_ADDED_TYPE = "condition_added";
    constexpr std::string_view CONNECTOR_REMOVED_TYPE = "connector_removed";
    constexpr std::string_view TRANSFORMATION_REMOVED_TYPE = "transformation_removed";
    constexpr std::string_view CONDITION_REMOVED_TYPE = "condition_removed";
    constexpr std::string_view ENTITY_OWNER_CHANGED_TYPE = "entity_owner_changed";

    constexpr std::string_view OBSERVED_STATE = "observed";
    constexpr std::string_view SAFE_STATE = "safe";
    constexpr std::string_view FINALIZED_STATE = "finalized";
    constexpr std::string_view REMOVED_STATE = "removed";

    enum class EventState : std::uint8_t
    {
        OBSERVED = 0,
        SAFE = 1,
        FINALIZED = 2,
        REMOVED = 3
    };

    enum class EventType : std::uint8_t
    {
        CONNECTOR_ADDED = 0,
        TRANSFORMATION_ADDED = 1,
        CONDITION_ADDED = 2,
        CONNECTOR_REMOVED = 3,
        TRANSFORMATION_REMOVED = 4,
        CONDITION_REMOVED = 5,
        ENTITY_OWNER_CHANGED = 6
    };

    struct RawChainLog
    {
        std::string source_id = "local";
        std::string chain_namespace = "local";
        std::uint64_t chain_id = 1;
        std::int64_t block_number = 0;
        std::string block_hash;
        std::string parent_hash;
        std::int64_t tx_index = 0;
        std::string tx_hash;
        std::int64_t log_index = 0;
        std::string address;
        std::array<std::optional<std::string>, 4> topics{};
        std::string data_hex;
        bool removed = false;
        std::optional<std::int64_t> block_time = std::nullopt;
        std::int64_t seen_at_ms = 0;
    };

    struct DecodedEvent
    {
        RawChainLog raw;
        EventType event_type = EventType::CONNECTOR_ADDED;
        EventState state = EventState::OBSERVED;

        std::string name;
        std::string caller;
        std::string owner;
        std::string entity_address;
        std::optional<std::uint32_t> args_count = std::nullopt;
        std::optional<std::string> format_hash = std::nullopt;
        std::optional<std::string> content_hash = std::nullopt;
        std::optional<std::string> metadata_hash = std::nullopt;
        std::optional<std::string> runtime_code_hash = std::nullopt;
        std::optional<pt::EntityKind> entity_kind = std::nullopt;
        std::optional<std::string> old_owner = std::nullopt;

        std::string decoded_json;
    };

    std::string toString(EventState state);
    std::string toString(EventType type);
}

namespace dcn::parse
{
    Result<dcn::events::EventState> parseEventState(const std::string & value);
    Result<dcn::events::EventType> parseEventType(const std::string & value);

    Result<dcn::events::RawChainLog> parseRawLog(
        const json & log_json,
        std::int64_t seen_at_ms,
        std::uint64_t chain_id,
        std::string source_id = "local",
        std::string chain_namespace = "local");
}
