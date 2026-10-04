#include <array>
#include <cstring>
#include <span>

#include "events_ingest.hpp"
#include "emitted_log_source.hpp"
#include "chain.hpp"
#include "parser.hpp"
#include "pt.hpp"
#include "registry_events.hpp"

namespace dcn::events
{
    namespace
    {
        std::optional<evmc::bytes32> readBytes32(const std::span<const std::uint8_t> data, const std::size_t offset)
        {
            if(offset > data.size() || data.size() - offset < 32) return std::nullopt;
            evmc::bytes32 value{};
            std::memcpy(value.bytes, data.data() + offset, 32);
            return value;
        }
    }

    std::string_view toString(const CursorKind kind)
    {
        switch(kind)
        {
        case CursorKind::LOCAL_SEQUENCE: return "local_sequence";
        case CursorKind::NEXT_BLOCK: return "next_block";
        }
        return "next_block";
    }

    std::optional<CursorKind> parseCursorKind(const std::string_view value)
    {
        if(value == "local_sequence") return CursorKind::LOCAL_SEQUENCE;
        if(value == "next_block") return CursorKind::NEXT_BLOCK;
        return std::nullopt;
    }

    PTEventDecoder::PTEventDecoder()
        : _connector_topic(pt::eventTopic(pt::CONNECTOR_ADDED_SIGNATURE))
        , _transformation_topic(pt::eventTopic(pt::TRANSFORMATION_ADDED_SIGNATURE))
        , _condition_topic(pt::eventTopic(pt::CONDITION_ADDED_SIGNATURE))
        , _connector_removed_topic(pt::eventTopic(pt::CONNECTOR_REMOVED_SIGNATURE))
        , _transformation_removed_topic(pt::eventTopic(pt::TRANSFORMATION_REMOVED_SIGNATURE))
        , _condition_removed_topic(pt::eventTopic(pt::CONDITION_REMOVED_SIGNATURE))
        , _owner_changed_topic(pt::eventTopic(pt::ENTITY_OWNER_CHANGED_SIGNATURE))
    {
    }

    std::optional<DecodedEvent> PTEventDecoder::decode(const RawChainLog& log) const
    {
        if (!log.topics[0].has_value())
        {
            return std::nullopt;
        }

        std::vector<std::string> topics_hex;
        topics_hex.reserve(log.topics.size());
        for (const auto& topic : log.topics)
        {
            if (topic.has_value())
            {
                topics_hex.push_back(*topic);
            }
        }

        const std::string topic0 = chain::normalizeHex(topics_hex.front());
        const EventState state = log.removed ? EventState::REMOVED : EventState::OBSERVED;

        if(topic0 == _connector_removed_topic || topic0 == _transformation_removed_topic ||
           topic0 == _condition_removed_topic)
        {
            const auto data = evmc::from_hex(log.data_hex);
            if(!data || data->size() < 32 * 6) return std::nullopt;
            const auto caller = chain::readAddressWord(data->data(), data->size(), 0);
            const auto owner = chain::readAddressWord(data->data(), data->size(), 32);
            const auto name_offset = chain::readWordAsSizeT(data->data(), data->size(), 64);
            const auto entity = chain::readAddressWord(data->data(), data->size(), 96);
            const auto content_hash = readBytes32(*data, 128);
            const auto metadata_hash = readBytes32(*data, 160);
            if(!caller || !owner || !name_offset || !entity || !content_hash || !metadata_hash) return std::nullopt;
            const auto name = chain::decodeAbiString(data->data(), data->size(), *name_offset);
            if(!name || !pt::isValidEntityName(*name)) return std::nullopt;

            const pt::EntityKind kind = topic0 == _connector_removed_topic
                ? pt::EntityKind::CONNECTOR
                : topic0 == _transformation_removed_topic
                    ? pt::EntityKind::TRANSFORMATION
                    : pt::EntityKind::CONDITION;
            const EventType type = kind == pt::EntityKind::CONNECTOR
                ? EventType::CONNECTOR_REMOVED
                : kind == pt::EntityKind::TRANSFORMATION
                    ? EventType::TRANSFORMATION_REMOVED
                    : EventType::CONDITION_REMOVED;

            return DecodedEvent{
                .raw = log,
                .event_type = type,
                .state = state,
                .name = *name,
                .caller = chain::addressToHex(*caller),
                .owner = chain::addressToHex(*owner),
                .entity_address = chain::addressToHex(*entity),
                .content_hash = chain::bytes32ToHex(*content_hash),
                .metadata_hash = chain::bytes32ToHex(*metadata_hash),
                .entity_kind = kind,
                .decoded_json = parse::dumpJsonLossy(json{{"name", *name}, {"owner", chain::addressToHex(*owner)}})
            };
        }

        if(topic0 == _owner_changed_topic)
        {
            const auto data = evmc::from_hex(log.data_hex);
            if(!data || data->size() < 32 * 5) return std::nullopt;
            const auto kind_id = chain::readUint32Word(data->data(), data->size(), 0);
            const auto name_offset = chain::readWordAsSizeT(data->data(), data->size(), 32);
            const auto entity = chain::readAddressWord(data->data(), data->size(), 64);
            const auto old_owner = chain::readAddressWord(data->data(), data->size(), 96);
            const auto new_owner = chain::readAddressWord(data->data(), data->size(), 128);
            if(!kind_id || *kind_id > 2 || !name_offset || !entity || !old_owner || !new_owner) return std::nullopt;
            const auto name = chain::decodeAbiString(data->data(), data->size(), *name_offset);
            if(!name || !pt::isValidEntityName(*name)) return std::nullopt;

            const auto kind = static_cast<pt::EntityKind>(*kind_id);
            return DecodedEvent{
                .raw = log,
                .event_type = EventType::ENTITY_OWNER_CHANGED,
                .state = state,
                .name = *name,
                .owner = chain::addressToHex(*new_owner),
                .entity_address = chain::addressToHex(*entity),
                .entity_kind = kind,
                .old_owner = chain::addressToHex(*old_owner),
                .decoded_json = parse::dumpJsonLossy(json{
                    {"kind", std::string(pt::entityKindName(kind))},
                    {"name", *name},
                    {"old_owner", chain::addressToHex(*old_owner)},
                    {"new_owner", chain::addressToHex(*new_owner)}})
            };
        }

        if (topic0 == _connector_topic)
        {
            const auto connector = pt::decodeConnectorAddedEvent(log.data_hex, topics_hex);
            if (!connector || !pt::isValidEntityName(connector->name))
            {
                return std::nullopt;
            }

            json payload {{"name", connector->name},
                          {"caller", chain::addressToHex(connector->caller)},
                          {"owner", chain::addressToHex(connector->owner)},
                          {"connector_address", chain::addressToHex(connector->connector_address)},
                          {"dimensions_count", connector->dimensions_count},
                          {"condition_name", connector->condition_name},
                          {"condition_args", connector->condition_args},
                          {"format_hash", chain::bytes32ToHex(connector->format_hash)},
                          {"content_hash", chain::bytes32ToHex(connector->content_hash)},
                          {"metadata_hash", chain::bytes32ToHex(connector->metadata_hash)},
                          {"runtime_code_hash", chain::bytes32ToHex(connector->runtime_code_hash)}};

            json composites = json::array();
            for (const auto& [dim_id, composite_name] : connector->composites)
            {
                composites.push_back(json {{"dim_id", dim_id}, {"composite_name", composite_name}});
            }
            payload["composites"] = std::move(composites);

            json bindings = json::array();
            for (const auto& [binding_key, binding_name] : connector->bindings)
            {
                bindings.push_back(
                    json {{"dim_id", binding_key.first}, {"slot_id", binding_key.second}, {"binding_name", binding_name}});
            }
            payload["bindings"] = std::move(bindings);

            json static_ri = json::array();
            for (const auto& [position, running_instance] : connector->static_ri)
            {
                static_ri.push_back(json {{"position", position},
                                          {"start_point", running_instance.first},
                                          {"transformation_shift", running_instance.second}});
            }
            payload["static_ri"] = std::move(static_ri);

            json transformations = json::array();
            for (const auto& [dim_id, defs] : connector->transformations)
                for (const auto& def : defs)
                {
                    json args = json::array();
                    for (int a = 0; a < def.args_size(); ++a) args.push_back(def.args(a));
                    transformations.push_back(json{{"dim_id", dim_id}, {"name", def.name()}, {"args", std::move(args)}});
                }
            payload["transformations"] = std::move(transformations);

            return DecodedEvent {.raw = log,
                                 .event_type = EventType::CONNECTOR_ADDED,
                                 .state = state,
                                 .name = connector->name,
                                 .caller = chain::addressToHex(connector->caller),
                                 .owner = chain::addressToHex(connector->owner),
                                 .entity_address = chain::addressToHex(connector->connector_address),
                                 .args_count = std::nullopt,
                                 .format_hash = chain::bytes32ToHex(connector->format_hash),
                                 .content_hash = chain::bytes32ToHex(connector->content_hash),
                                 .metadata_hash = chain::bytes32ToHex(connector->metadata_hash),
                                 .runtime_code_hash = chain::bytes32ToHex(connector->runtime_code_hash),
                                 .entity_kind = pt::EntityKind::CONNECTOR,
                                 .decoded_json = parse::dumpJsonLossy(payload)};
        }

        if (topic0 == _transformation_topic)
        {
            const auto transformation = pt::decodeTransformationAddedEvent(log.data_hex, topics_hex);
            if (!transformation || !pt::isValidEntityName(transformation->name))
            {
                return std::nullopt;
            }

            json payload {{"name", transformation->name},
                          {"caller", chain::addressToHex(transformation->caller)},
                          {"owner", chain::addressToHex(transformation->owner)},
                          {"transformation_address", chain::addressToHex(transformation->transformation_address)},
                          {"args_count", transformation->args_count},
                          {"content_hash", chain::bytes32ToHex(transformation->content_hash)},
                          {"metadata_hash", chain::bytes32ToHex(transformation->metadata_hash)},
                          {"runtime_code_hash", chain::bytes32ToHex(transformation->runtime_code_hash)}};

            return DecodedEvent {.raw = log,
                                 .event_type = EventType::TRANSFORMATION_ADDED,
                                 .state = state,
                                 .name = transformation->name,
                                 .caller = chain::addressToHex(transformation->caller),
                                 .owner = chain::addressToHex(transformation->owner),
                                 .entity_address = chain::addressToHex(transformation->transformation_address),
                                 .args_count = transformation->args_count,
                                 .format_hash = std::nullopt,
                                 .content_hash = chain::bytes32ToHex(transformation->content_hash),
                                 .metadata_hash = chain::bytes32ToHex(transformation->metadata_hash),
                                 .runtime_code_hash = chain::bytes32ToHex(transformation->runtime_code_hash),
                                 .entity_kind = pt::EntityKind::TRANSFORMATION,
                                 .decoded_json = parse::dumpJsonLossy(payload)};
        }

        if (topic0 == _condition_topic)
        {
            const auto condition = pt::decodeConditionAddedEvent(log.data_hex, topics_hex);
            if (!condition || !pt::isValidEntityName(condition->name))
            {
                return std::nullopt;
            }

            json payload {{"name", condition->name},
                          {"caller", chain::addressToHex(condition->caller)},
                          {"owner", chain::addressToHex(condition->owner)},
                          {"condition_address", chain::addressToHex(condition->condition_address)},
                          {"args_count", condition->args_count},
                          {"content_hash", chain::bytes32ToHex(condition->content_hash)},
                          {"metadata_hash", chain::bytes32ToHex(condition->metadata_hash)},
                          {"runtime_code_hash", chain::bytes32ToHex(condition->runtime_code_hash)}};

            return DecodedEvent {.raw = log,
                                 .event_type = EventType::CONDITION_ADDED,
                                 .state = state,
                                 .name = condition->name,
                                 .caller = chain::addressToHex(condition->caller),
                                 .owner = chain::addressToHex(condition->owner),
                                 .entity_address = chain::addressToHex(condition->condition_address),
                                 .args_count = condition->args_count,
                                 .format_hash = std::nullopt,
                                 .content_hash = chain::bytes32ToHex(condition->content_hash),
                                 .metadata_hash = chain::bytes32ToHex(condition->metadata_hash),
                                 .runtime_code_hash = chain::bytes32ToHex(condition->runtime_code_hash),
                                 .entity_kind = pt::EntityKind::CONDITION,
                                 .decoded_json = parse::dumpJsonLossy(payload)};
        }

        return std::nullopt;
    }
} // namespace dcn::events
