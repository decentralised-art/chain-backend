#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <absl/hash/hash.h>

#include "connector.pb.h"

#include "parser.hpp"
#include "chain.hpp"

namespace dcn
{
    template <typename H>
    inline H AbslHashValue(H h, const TransformationDef & td)
    {
        h = H::combine(std::move(h), td.name());
        for(const int32_t & arg : td.args())
        {
            h = H::combine(std::move(h), arg);
        }
        return h;
    }

    template <typename H>
    inline H AbslHashValue(H h, const Dimension & d)
    {
        h = H::combine(std::move(h), d.composite());

        std::vector<std::pair<std::string, std::string>> sorted_bindings;
        sorted_bindings.reserve(static_cast<std::size_t>(d.bindings_size()));
        for(const auto & [slot, composite] : d.bindings())
        {
            sorted_bindings.emplace_back(slot, composite);
        }
        std::ranges::sort(sorted_bindings, [](const auto & lhs, const auto & rhs)
        {
            return lhs.first < rhs.first;
        });

        for(const auto & [slot, composite] : sorted_bindings)
        {
            h = H::combine(std::move(h), slot, composite);
        }

        for(const TransformationDef & t : d.transformations())
        {
            h = H::combine(std::move(h), t);
        }
        return h;
    }

    template <typename H>
    inline H AbslHashValue(H h, const Connector & p)
    {
        h = H::combine(std::move(h), p.name());

        for(const Dimension & d : p.dimensions())
        {
            h = H::combine(std::move(h), d);
        }

        h = H::combine(std::move(h), p.condition_name());
        for(const auto & arg : p.condition_args())
        {
            h = H::combine(std::move(h), arg);
        }
        return h;
    }

    parse::Result<std::string> constructConnectorSolidityCode(const Connector & connector);
}

namespace dcn::pt
{
    struct ConnectorAddedEvent
    {
        chain::Address caller{};
        chain::Address owner{};
        std::string name;
        chain::Address connector_address{};
        std::uint32_t dimensions_count{};
        std::map<std::uint32_t, std::string> composites;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> bindings;
        std::string condition_name;
        std::vector<std::int32_t> condition_args;
        evmc::bytes32 format_hash{};
        // Static running instances keyed by local position id -> {start_point, transformation_shift}.
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> static_ri;
        // Per-dimension transformation defs, reconstructed from the log (key = dim id, value ordered as emitted).
        std::map<std::uint32_t, std::vector<TransformationDef>> transformations;
        evmc::bytes32 content_hash{};
        evmc::bytes32 metadata_hash{};
        evmc::bytes32 runtime_code_hash{};
    };

    std::optional<ConnectorAddedEvent> decodeConnectorAddedEvent(
        const std::uint8_t* data,
        std::size_t data_size,
        const evmc::bytes32 topics[],
        std::size_t num_topics);

    std::optional<ConnectorAddedEvent> decodeConnectorAddedEvent(
        const std::string & data_hex,
        const std::vector<std::string> & topics_hex);

    // Reconstruct a full ConnectorRecord from a decoded ConnectorAdded event.
    // Maps name, owner (hex), dims (transformations + composite + bindings),
    // condition name/args, and static_ri back into the Connector proto.
    ConnectorRecord buildConnectorRecordFromEvent(const ConnectorAddedEvent & event);
}

namespace dcn::parse
{
    template<>
    Result<json> parseToJson(TransformationDef transform_def, use_json_t);

    template<>
    Result<TransformationDef> parseFromJson(json json_obj, use_json_t);

    template<>
    Result<json> parseToJson(Dimension dimension, use_json_t);

    template<>
    Result<Dimension> parseFromJson(json json_obj, use_json_t);

    template<>
    Result<std::string> parseToJson(Dimension dimension, use_protobuf_t);

    template<>
    Result<Dimension> parseFromJson(std::string json_str, use_protobuf_t);

    template<>
    Result<json> parseToJson(Connector connector, use_json_t);

    template<>
    Result<Connector> parseFromJson(json json_obj, use_json_t);

    template<>
    Result<std::string> parseToJson(Connector connector, use_protobuf_t);

    template<>
    Result<Connector> parseFromJson(std::string json_str, use_protobuf_t);

    template<>
    Result<json> parseToJson(ConnectorRecord connector_record, use_json_t);

    template<>
    Result<std::string> parseToJson(ConnectorRecord connector_record, use_protobuf_t);

    template<>
    Result<ConnectorRecord> parseFromJson(json json_obj, use_json_t);

    template<>
    Result<ConnectorRecord> parseFromJson(std::string json_str, use_protobuf_t);
}
