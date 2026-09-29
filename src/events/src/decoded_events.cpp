#include <string_view>

#include "decoded_event.hpp"
#include "chain.hpp"

namespace dcn::events
{
    std::string toString(const EventState state)
    {
        switch(state)
        {
        case EventState::OBSERVED:
            return std::string(OBSERVED_STATE);
        case EventState::SAFE:
            return std::string(SAFE_STATE);
        case EventState::FINALIZED:
            return std::string(FINALIZED_STATE);
        case EventState::REMOVED:
            return std::string(REMOVED_STATE);
        }

        assert(false);
    }

    std::string toString(const EventType type)
    {
        switch(type)
        {
        case EventType::CONNECTOR_ADDED:
            return std::string(CONNECTOR_ADDED_TYPE);
        case EventType::TRANSFORMATION_ADDED:
            return std::string(TRANSFORMATION_ADDED_TYPE);
        case EventType::CONDITION_ADDED:
            return std::string(CONDITION_ADDED_TYPE);
        case EventType::CONNECTOR_REMOVED:
            return std::string(CONNECTOR_REMOVED_TYPE);
        case EventType::TRANSFORMATION_REMOVED:
            return std::string(TRANSFORMATION_REMOVED_TYPE);
        case EventType::CONDITION_REMOVED:
            return std::string(CONDITION_REMOVED_TYPE);
        case EventType::ENTITY_OWNER_CHANGED:
            return std::string(ENTITY_OWNER_CHANGED_TYPE);
        }

        assert(false);
    }
}

namespace dcn::parse
{
    Result<events::EventState> parseEventState(const std::string & value)
    {
        if(value == events::OBSERVED_STATE)
        {
            return events::EventState::OBSERVED;
        }
        if(value == events::SAFE_STATE)
        {
            return events::EventState::SAFE;
        }
        if(value == events::FINALIZED_STATE)
        {
            return events::EventState::FINALIZED;
        }
        if(value == events::REMOVED_STATE)
        {
            return events::EventState::REMOVED;
        }
        
        return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
    }

    Result<events::EventType> parseEventType(const std::string & value)
    {
        if(value == events::CONNECTOR_ADDED_TYPE)
        {
            return events::EventType::CONNECTOR_ADDED;
        }
        if(value == events::TRANSFORMATION_ADDED_TYPE)
        {
            return events::EventType::TRANSFORMATION_ADDED;
        }
        if(value == events::CONDITION_ADDED_TYPE)
        {
            return events::EventType::CONDITION_ADDED;
        }
        if(value == events::CONNECTOR_REMOVED_TYPE) return events::EventType::CONNECTOR_REMOVED;
        if(value == events::TRANSFORMATION_REMOVED_TYPE) return events::EventType::TRANSFORMATION_REMOVED;
        if(value == events::CONDITION_REMOVED_TYPE) return events::EventType::CONDITION_REMOVED;
        if(value == events::ENTITY_OWNER_CHANGED_TYPE) return events::EventType::ENTITY_OWNER_CHANGED;

        return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
    }

    Result<dcn::events::RawChainLog> parseRawLog(
        const json & log_json,
        const std::int64_t seen_at_ms,
        std::uint64_t chain_id,
        std::string source_id,
        std::string chain_namespace)
    {
        if(!log_json.is_object())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        }

        if(!log_json.contains("blockNumber") || !log_json["blockNumber"].is_string() ||
           !log_json.contains("blockHash") || !log_json["blockHash"].is_string() ||
           !log_json.contains("transactionHash") || !log_json["transactionHash"].is_string() ||
           !log_json.contains("transactionIndex") || !log_json["transactionIndex"].is_string() ||
           !log_json.contains("logIndex") || !log_json["logIndex"].is_string() ||
           !log_json.contains("address") || !log_json["address"].is_string() ||
           !log_json.contains("data") || !log_json["data"].is_string() ||
           !log_json.contains("topics") || !log_json["topics"].is_array())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        }

        const parse::Result<std::int64_t> block_number = parse::parseHexQuantity(log_json["blockNumber"].get<std::string>());
        const parse::Result<std::int64_t> tx_index = parse::parseHexQuantity(log_json["transactionIndex"].get<std::string>());
        const parse::Result<std::int64_t> log_index = parse::parseHexQuantity(log_json["logIndex"].get<std::string>());

        if(!block_number.has_value() || !tx_index.has_value() || !log_index.has_value())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        }

        events::RawChainLog log{
            .source_id = std::move(source_id),
            .chain_namespace = std::move(chain_namespace),
            .chain_id = chain_id,
            .block_number = *block_number,
            .block_hash = chain::normalizeHex(log_json["blockHash"].get<std::string>()),
            .parent_hash = {},
            .tx_index = *tx_index,
            .tx_hash = chain::normalizeHex(log_json["transactionHash"].get<std::string>()),
            .log_index = *log_index,
            .address = chain::normalizeHex(log_json["address"].get<std::string>()),
            .topics = {},
            .data_hex = chain::normalizeHex(log_json["data"].get<std::string>()),
            .removed = log_json.value("removed", false),
            .block_time = std::nullopt,
            .seen_at_ms = seen_at_ms
        };

        const auto & topics = log_json["topics"];
        for(std::size_t i = 0; i < std::min<std::size_t>(topics.size(), log.topics.size()); ++i)
        {
            if(!topics.at(i).is_string())
            {
                continue;
            }
            log.topics[i] = chain::normalizeHex(topics.at(i).get<std::string>());
        }
        return log;
    }
}
