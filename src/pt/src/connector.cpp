#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <vector>

#include <spdlog/spdlog.h>

#include "crypto.hpp"

#include "connector.hpp"
#include "solidity_identifier.hpp"

namespace dcn
{
    namespace
    {
        std::string _escapeSolidityStringLiteral(std::string_view value)
        {
            auto toHex = [](std::uint8_t nibble) -> char
            {
                return static_cast<char>((nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10)));
            };

            std::string escaped;
            escaped.reserve(value.size());

            for(unsigned char ch : value)
            {
                switch(ch)
                {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    if(ch < 0x20 || ch >= 0x7F)
                    {
                        escaped += "\\x";
                        escaped.push_back(toHex(static_cast<std::uint8_t>((ch >> 4) & 0x0F)));
                        escaped.push_back(toHex(static_cast<std::uint8_t>(ch & 0x0F)));
                    }
                    else
                    {
                        escaped.push_back(static_cast<char>(ch));
                    }
                    break;
                }
            }

            return escaped;
        }

    }

    parse::Result<std::string> constructConnectorSolidityCode(const Connector & connector)
    {
        if(connector.dimensions_size() <= 0)
        {
            spdlog::error("Connector `{}` has no dimensions", connector.name());
            return std::unexpected(parse::ParseError{
                parse::ParseError::Kind::INVALID_VALUE,
                std::format("Connector `{}` has no dimensions", connector.name())});
        }

        if(!pt::isValidSolidityIdentifier(connector.name()))
        {
            spdlog::error("Connector `{}` has invalid Solidity identifier name", connector.name());
            return std::unexpected(parse::ParseError{
                parse::ParseError::Kind::INVALID_VALUE,
                std::format("Connector `{}` has invalid Solidity identifier name", connector.name())});
        }

        std::string composite_dim_ids_code;
        std::string composite_names_code;
        std::string binding_dim_ids_code;
        std::string binding_slot_ids_code;
        std::string binding_names_code;
        std::string static_ri_positions_code;
        std::string static_ri_start_points_code;
        std::string static_ri_transform_shifts_code;
        std::string condition_args_code;
        std::string transform_def_code;

        std::vector<std::pair<std::uint32_t, std::string>> sorted_composites;
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>> sorted_bindings;
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> sorted_static_ri;
        std::set<std::pair<std::uint32_t, std::uint32_t>> canonical_binding_slots;
        const std::string escaped_connector_name = _escapeSolidityStringLiteral(connector.name());
        const std::string escaped_condition_name = _escapeSolidityStringLiteral(connector.condition_name());

        std::size_t total_bindings_count = 0;
        for(int dim_idx = 0; dim_idx < connector.dimensions_size(); ++dim_idx)
        {
            const Dimension & dimension = connector.dimensions(dim_idx);
            total_bindings_count += static_cast<std::size_t>(dimension.bindings_size());

            for(int tx_idx = 0; tx_idx < dimension.transformations_size(); ++tx_idx)
            {
                const TransformationDef & transformation = dimension.transformations(tx_idx);
                if(transformation.name().empty())
                {
                    spdlog::error(
                        "Connector `{}` has transformation with empty name at dimension {} index {}",
                        connector.name(),
                        dim_idx,
                        tx_idx);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format(
                            "Connector `{}` has transformation with empty name at dimension {} index {}",
                            connector.name(),
                            dim_idx,
                            tx_idx)});
                }
            }
        }

        sorted_composites.reserve(static_cast<std::size_t>(connector.dimensions_size()));
        sorted_bindings.reserve(total_bindings_count);
        for(std::uint32_t dim_id = 0; dim_id < static_cast<std::uint32_t>(connector.dimensions_size()); ++dim_id)
        {
            const Dimension & dimension = connector.dimensions(static_cast<int>(dim_id));
            const std::string & composite_name = dimension.composite();
            if(composite_name.empty())
            {
                if(dimension.bindings_size() != 0)
                {
                    spdlog::error("Connector `{}` has bindings on scalar dimension {}", connector.name(), dim_id);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format("Connector `{}` has bindings on scalar dimension {}", connector.name(), dim_id)});
                }
            }
            else
            {
                if(!pt::isValidSolidityIdentifier(composite_name))
                {
                    spdlog::error(
                        "Connector `{}` has invalid composite connector identifier `{}` at dim {}",
                        connector.name(),
                        composite_name,
                        dim_id);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format(
                            "Connector `{}` has invalid composite connector identifier `{}` at dim {}",
                            connector.name(),
                            composite_name,
                            dim_id)});
                }
                sorted_composites.emplace_back(dim_id, composite_name);
            }

            for(const auto & [slot, binding_name] : dimension.bindings())
            {
                if(binding_name.empty())
                {
                    spdlog::error("Connector `{}` has empty binding target at dim {} slot `{}`", connector.name(), dim_id, slot);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format("Connector `{}` has empty binding target at dim {} slot `{}`", connector.name(), dim_id, slot)});
                }
                if(!pt::isValidSolidityIdentifier(binding_name))
                {
                    spdlog::error(
                        "Connector `{}` has invalid binding target identifier `{}` at dim {} slot `{}`",
                        connector.name(),
                        binding_name,
                        dim_id,
                        slot);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format(
                            "Connector `{}` has invalid binding target identifier `{}` at dim {} slot `{}`",
                            connector.name(),
                            binding_name,
                            dim_id,
                            slot)});
                }

                const auto slot_id = parse::parseUint32Decimal(slot);
                if(!slot_id)
                {
                    spdlog::error("Connector `{}` has non-numeric binding slot `{}` at dim {}", connector.name(), slot, dim_id);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format("Connector `{}` has non-numeric binding slot `{}` at dim {}", connector.name(), slot, dim_id)});
                }
                if(slot != std::to_string(*slot_id))
                {
                    spdlog::error("Connector `{}` has non-canonical binding slot key `{}` at dim {}", connector.name(), slot, dim_id);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format("Connector `{}` has non-canonical binding slot key `{}` at dim {}", connector.name(), slot, dim_id)});
                }

                const auto canonical_slot = std::make_pair(dim_id, *slot_id);
                if(!canonical_binding_slots.insert(canonical_slot).second)
                {
                    spdlog::error("Connector `{}` has duplicate binding slot id {} at dim {}", connector.name(), *slot_id, dim_id);
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        std::format("Connector `{}` has duplicate binding slot id {} at dim {}", connector.name(), *slot_id, dim_id)});
                }

                sorted_bindings.emplace_back(dim_id, *slot_id, binding_name);
            }
        }
        std::ranges::sort(sorted_composites, [](const auto & lhs, const auto & rhs)
        {
            return lhs.first < rhs.first;
        });
        std::ranges::sort(sorted_bindings, [](const auto & lhs, const auto & rhs)
        {
            if(std::get<0>(lhs) != std::get<0>(rhs))
            {
                return std::get<0>(lhs) < std::get<0>(rhs);
            }
            if(std::get<1>(lhs) != std::get<1>(rhs))
            {
                return std::get<1>(lhs) < std::get<1>(rhs);
            }
            return std::get<2>(lhs) < std::get<2>(rhs);
        });

        sorted_static_ri.reserve(static_cast<std::size_t>(connector.static_ri_size()));
        for(const auto & [position, running_instance] : connector.static_ri())
        {
            sorted_static_ri.emplace_back(
                position,
                running_instance.start_point(),
                running_instance.transformation_shift());
        }
        std::ranges::sort(sorted_static_ri, [](const auto & lhs, const auto & rhs)
        {
            return std::get<0>(lhs) < std::get<0>(rhs);
        });

        for(int i = 0; i < connector.condition_args_size(); ++i)
        {
            condition_args_code += std::format(
                "conditionArgs[{}] = int32({});\n",
                i,
                connector.condition_args().at(i));
        }

        for(std::size_t i = 0; i < sorted_composites.size(); ++i)
        {
            composite_dim_ids_code += std::format("compositeDimIds[{}] = uint32({});\n", i, sorted_composites[i].first);
            composite_names_code += std::format(
                "compositeNames[{}] = \"{}\";\n",
                i,
                _escapeSolidityStringLiteral(sorted_composites[i].second));
        }

        for(std::size_t i = 0; i < sorted_bindings.size(); ++i)
        {
            binding_dim_ids_code += std::format("bindingDimIds[{}] = uint32({});\n", i, std::get<0>(sorted_bindings[i]));
            binding_slot_ids_code += std::format("bindingSlotIds[{}] = uint32({});\n", i, std::get<1>(sorted_bindings[i]));
            binding_names_code += std::format(
                "bindingNames[{}] = \"{}\";\n",
                i,
                _escapeSolidityStringLiteral(std::get<2>(sorted_bindings[i])));
        }

        for(std::size_t i = 0; i < sorted_static_ri.size(); ++i)
        {
            static_ri_positions_code += std::format(
                "staticRiPositions[{}] = uint32({});\n",
                i,
                std::get<0>(sorted_static_ri[i]));
            static_ri_start_points_code += std::format(
                "staticRiStartPoints[{}] = uint32({});\n",
                i,
                std::get<1>(sorted_static_ri[i]));
            static_ri_transform_shifts_code += std::format(
                "staticRiTransformShifts[{}] = uint32({});\n",
                i,
                std::get<2>(sorted_static_ri[i]));
        }

        for(int i = 0; i < connector.dimensions_size(); ++i)
        {
            for(int j = 0; j < connector.dimensions(i).transformations_size(); ++j)
            {
                const auto & transform = connector.dimensions(i).transformations(j);
                if(transform.args_size() == 0)
                {
                    transform_def_code += std::format(
                        "getCallDef().push({}, \"{}\");\n",
                        i,
                        _escapeSolidityStringLiteral(transform.name()));
                    continue;
                }

                std::string transform_args_code;
                for(int k = 0; k < transform.args_size(); ++k)
                {
                    transform_args_code += std::format("uint32({})", transform.args(k));
                    if(k + 1 != transform.args_size())
                    {
                        transform_args_code += ", ";
                    }
                }

                transform_def_code += std::format(
                    "getCallDef().push({}, \"{}\", [{}]);\n",
                    i,
                    _escapeSolidityStringLiteral(transform.name()),
                    transform_args_code);
            }
        }


        return std::format(
            "//SPDX-License-Identifier: MIT\n"
            "pragma solidity >=0.8.2 <0.9.0;\n"
            "import \"connector/ConnectorBase.sol\";\n"
            "contract {0} is ConnectorBase{{\n"
            "function _compositeDimIds() internal pure returns (uint32[] memory compositeDimIds) {{"
            "compositeDimIds = new uint32[]({1});"
            "{2}"
            "}}\n"
            "function _compositeNames() internal pure returns (string[] memory compositeNames) {{"
            "compositeNames = new string[]({1});"
            "{3}"
            "}}\n"
            "function _bindingDimIds() internal pure returns (uint32[] memory bindingDimIds) {{"
            "bindingDimIds = new uint32[]({4});"
            "{5}"
            "}}\n"
            "function _bindingSlotIds() internal pure returns (uint32[] memory bindingSlotIds) {{"
            "bindingSlotIds = new uint32[]({4});"
            "{6}"
            "}}\n"
            "function _bindingNames() internal pure returns (string[] memory bindingNames) {{"
            "bindingNames = new string[]({4});"
            "{7}"
            "}}\n"
            "function _staticRiPositions() internal pure returns (uint32[] memory staticRiPositions) {{"
            "staticRiPositions = new uint32[]({8});"
            "{9}"
            "}}\n"
            "function _staticRiStartPoints() internal pure returns (uint32[] memory staticRiStartPoints) {{"
            "staticRiStartPoints = new uint32[]({8});"
            "{10}"
            "}}\n"
            "function _staticRiTransformShifts() internal pure returns (uint32[] memory staticRiTransformShifts) {{"
            "staticRiTransformShifts = new uint32[]({8});"
            "{11}"
            "}}\n"
            "function _conditionArgs() internal pure returns (int32[] memory conditionArgs) {{"
            "conditionArgs = new int32[]({12});"
            "{13}"
            "}}\n"
            "constructor(address registryAddr, bytes32 contentHash_, bytes32 metadataHash_) "
            "ConnectorBase(registryAddr, \"{17}\", {14}, contentHash_, metadataHash_) {{\n"
            "{15}"
            "__ConnectorBase_finalizeInit(_compositeDimIds(), _compositeNames(), _bindingDimIds(), _bindingSlotIds(), _bindingNames(), _staticRiPositions(), _staticRiStartPoints(), _staticRiTransformShifts(), \"{16}\", _conditionArgs());\n"
            "}}\n"
            "}}",

            connector.name(),                         // 0
            sorted_composites.size(),                 // 1
            std::move(composite_dim_ids_code),        // 2
            std::move(composite_names_code),          // 3
            sorted_bindings.size(),                   // 4
            std::move(binding_dim_ids_code),          // 5
            std::move(binding_slot_ids_code),         // 6
            std::move(binding_names_code),            // 7
            sorted_static_ri.size(),                  // 8
            std::move(static_ri_positions_code),      // 9
            std::move(static_ri_start_points_code),   // 10
            std::move(static_ri_transform_shifts_code), // 11
            connector.condition_args_size(),          // 12
            std::move(condition_args_code),           // 13
            connector.dimensions_size(),              // 14
            std::move(transform_def_code),            // 15
            escaped_condition_name,                   // 16
            escaped_connector_name                    // 17
        );
    }
}

namespace dcn::pt
{
    std::optional<ConnectorAddedEvent> decodeConnectorAddedEvent(
        const std::uint8_t* data,
        std::size_t data_size,
        const evmc::bytes32 topics[],
        std::size_t num_topics)
    {
        if(data == nullptr || topics == nullptr || num_topics < 3 || data_size < 32 * 21)
        {
            return std::nullopt;
        }

        const evmc::bytes32 expected_topic = chain::constructEventTopic(
            "ConnectorAdded(address,address,string,address,uint32,uint32[],string[],uint32[],uint32[],string[],string,int32[],bytes32,uint32[],uint32[],uint32[],uint32[],string[],uint32[],int32[],bytes32,bytes32,bytes32)");

        if(topics[0] != expected_topic)
        {
            return std::nullopt;
        }

        ConnectorAddedEvent event{};
        event.caller = chain::topicWordToAddress(topics[1]);
        event.owner = chain::topicWordToAddress(topics[2]);

        const auto name_offset = chain::readWordAsSizeT(data, data_size, 0);
        const auto connector_address = chain::readAddressWord(data, data_size, 32);
        const auto dimensions_count = chain::readUint32Word(data, data_size, 64);
        const auto composite_dim_ids_offset = chain::readWordAsSizeT(data, data_size, 96);
        const auto composite_names_offset = chain::readWordAsSizeT(data, data_size, 128);
        const auto binding_dim_ids_offset = chain::readWordAsSizeT(data, data_size, 160);
        const auto binding_slot_ids_offset = chain::readWordAsSizeT(data, data_size, 192);
        const auto binding_names_offset = chain::readWordAsSizeT(data, data_size, 224);
        const auto condition_offset = chain::readWordAsSizeT(data, data_size, 256);
        const auto condition_args_offset = chain::readWordAsSizeT(data, data_size, 288);
        evmc::bytes32 format_hash_word{};
        if(320 + 32 > data_size)
        {
            return std::nullopt;
        }
        std::memcpy(format_hash_word.bytes, data + 320, 32);

        const auto static_ri_positions_offset = chain::readWordAsSizeT(data, data_size, 352);
        const auto static_ri_start_points_offset = chain::readWordAsSizeT(data, data_size, 384);
        const auto static_ri_transform_shifts_offset = chain::readWordAsSizeT(data, data_size, 416);

        // Four new dynamic-array head pointers appended after staticRiTransformShifts.
        const auto transformation_dim_ids_offset = chain::readWordAsSizeT(data, data_size, 448);
        const auto transformation_names_offset = chain::readWordAsSizeT(data, data_size, 480);
        const auto transformation_arg_counts_offset = chain::readWordAsSizeT(data, data_size, 512);
        const auto transformation_args_offset = chain::readWordAsSizeT(data, data_size, 544);

        // Publication hashes, appended after the reconstruction arrays.
        std::memcpy(event.content_hash.bytes, data + 576, 32);
        std::memcpy(event.metadata_hash.bytes, data + 608, 32);
        std::memcpy(event.runtime_code_hash.bytes, data + 640, 32);

        if(!name_offset || !connector_address || !dimensions_count || !composite_dim_ids_offset || !composite_names_offset || !binding_dim_ids_offset || !binding_slot_ids_offset || !binding_names_offset || !condition_offset || !condition_args_offset || !static_ri_positions_offset || !static_ri_start_points_offset || !static_ri_transform_shifts_offset || !transformation_dim_ids_offset || !transformation_names_offset || !transformation_arg_counts_offset || !transformation_args_offset)
        {
            return std::nullopt;
        }

        if(*dimensions_count == 0)
        {
            return std::nullopt;
        }

        const auto name = chain::decodeAbiString(data, data_size, *name_offset);
        const auto composite_dim_ids = chain::decodeAbiUint32Array(data, data_size, *composite_dim_ids_offset);
        const auto composite_names = chain::decodeAbiStringArray(data, data_size, *composite_names_offset);
        const auto binding_dim_ids = chain::decodeAbiUint32Array(data, data_size, *binding_dim_ids_offset);
        const auto binding_slot_ids = chain::decodeAbiUint32Array(data, data_size, *binding_slot_ids_offset);
        const auto binding_names = chain::decodeAbiStringArray(data, data_size, *binding_names_offset);
        const auto condition_name = chain::decodeAbiString(data, data_size, *condition_offset);
        const auto condition_args = chain::decodeAbiInt32Array(data, data_size, *condition_args_offset);
        const auto static_ri_positions = chain::decodeAbiUint32Array(data, data_size, *static_ri_positions_offset);
        const auto static_ri_start_points = chain::decodeAbiUint32Array(data, data_size, *static_ri_start_points_offset);
        const auto static_ri_transform_shifts = chain::decodeAbiUint32Array(data, data_size, *static_ri_transform_shifts_offset);
        const auto transformation_dim_ids = chain::decodeAbiUint32Array(data, data_size, *transformation_dim_ids_offset);
        const auto transformation_names = chain::decodeAbiStringArray(data, data_size, *transformation_names_offset);
        const auto transformation_arg_counts = chain::decodeAbiUint32Array(data, data_size, *transformation_arg_counts_offset);
        const auto transformation_args = chain::decodeAbiInt32Array(data, data_size, *transformation_args_offset);

        if(!name || !composite_dim_ids || !composite_names || !binding_dim_ids || !binding_slot_ids || !binding_names || !condition_name || !condition_args || !static_ri_positions || !static_ri_start_points || !static_ri_transform_shifts || !transformation_dim_ids || !transformation_names || !transformation_arg_counts || !transformation_args)
        {
            return std::nullopt;
        }

        if(static_ri_positions->size() != static_ri_start_points->size() || static_ri_positions->size() != static_ri_transform_shifts->size())
        {
            return std::nullopt;
        }

        if(transformation_dim_ids->size() != transformation_names->size() || transformation_dim_ids->size() != transformation_arg_counts->size())
        {
            return std::nullopt;
        }

        if(composite_dim_ids->size() != composite_names->size())
        {
            return std::nullopt;
        }

        if(binding_dim_ids->size() != binding_slot_ids->size() || binding_dim_ids->size() != binding_names->size())
        {
            return std::nullopt;
        }

        event.name = *name;
        event.connector_address = *connector_address;
        event.dimensions_count = *dimensions_count;
        event.condition_name = *condition_name;
        event.condition_args = *condition_args;
        event.format_hash = format_hash_word;

        for(std::size_t i = 0; i < composite_dim_ids->size(); ++i)
        {
            if(composite_names->at(i).empty())
            {
                return std::nullopt;
            }

            if(composite_dim_ids->at(i) >= event.dimensions_count)
            {
                return std::nullopt;
            }

            const auto [_, inserted] = event.composites.try_emplace(composite_dim_ids->at(i), composite_names->at(i));
            if(!inserted)
            {
                return std::nullopt;
            }
        }

        for(std::size_t i = 0; i < binding_dim_ids->size(); ++i)
        {
            if(binding_names->at(i).empty())
            {
                return std::nullopt;
            }

            if(binding_dim_ids->at(i) >= event.dimensions_count)
            {
                return std::nullopt;
            }

            const auto [_, inserted] = event.bindings.try_emplace(
                std::make_pair(binding_dim_ids->at(i), binding_slot_ids->at(i)),
                binding_names->at(i));
            if(!inserted)
            {
                return std::nullopt;
            }
        }

        for(std::size_t i = 0; i < static_ri_positions->size(); ++i)
        {
            const auto [_, inserted] = event.static_ri.try_emplace(
                static_ri_positions->at(i),
                std::make_pair(static_ri_start_points->at(i), static_ri_transform_shifts->at(i)));
            if(!inserted)
            {
                return std::nullopt;
            }
        }

        // Walk the parallel transformation arrays, consuming the flattened args left-to-right
        // by transformationArgCounts[k], grouping into transformations[transformationDimIds[k]].
        std::size_t transformation_arg_cursor = 0;
        for(std::size_t k = 0; k < transformation_dim_ids->size(); ++k)
        {
            const std::uint32_t dim_id = transformation_dim_ids->at(k);
            if(dim_id >= event.dimensions_count)
            {
                return std::nullopt;
            }

            const std::size_t arg_count = transformation_arg_counts->at(k);
            if(arg_count > transformation_args->size() - transformation_arg_cursor)
            {
                return std::nullopt;
            }

            TransformationDef transformation;
            transformation.set_name(transformation_names->at(k));
            for(std::size_t a = 0; a < arg_count; ++a)
            {
                transformation.add_args(transformation_args->at(transformation_arg_cursor + a));
            }
            transformation_arg_cursor += arg_count;

            event.transformations[dim_id].push_back(std::move(transformation));
        }

        if(transformation_arg_cursor != transformation_args->size())
        {
            return std::nullopt;
        }

        return event;
    }

    std::optional<ConnectorAddedEvent> decodeConnectorAddedEvent(
        const std::string & data_hex,
        const std::vector<std::string> & topics_hex)
    {
        const auto data_bytes = evmc::from_hex(data_hex);
        if(!data_bytes)
        {
            return std::nullopt;
        }

        const auto topic_words = chain::decodeTopicWords(topics_hex);
        if(!topic_words)
        {
            return std::nullopt;
        }

        return decodeConnectorAddedEvent(
            data_bytes->data(),
            data_bytes->size(),
            topic_words->data(),
            topic_words->size());
    }

    ConnectorRecord buildConnectorRecordFromEvent(const ConnectorAddedEvent & event)
    {
        ConnectorRecord record;
        record.set_owner(chain::addressToHex(event.owner));

        Connector * connector = record.mutable_connector();
        connector->set_name(event.name);
        connector->set_condition_name(event.condition_name);
        for(const std::int32_t arg : event.condition_args)
        {
            connector->add_condition_args(arg);
        }

        // Allocate dims [0, dimensions_count), then fill each from the parallel maps.
        for(std::uint32_t dim = 0; dim < event.dimensions_count; ++dim)
        {
            Dimension * dimension = connector->add_dimensions();

            if(const auto it = event.transformations.find(dim); it != event.transformations.end())
            {
                for(const auto & transformation : it->second)
                {
                    *dimension->add_transformations() = transformation;
                }
            }

            if(const auto it = event.composites.find(dim); it != event.composites.end())
            {
                dimension->set_composite(it->second);
            }

            for(const auto & [key, name] : event.bindings)
            {
                if(key.first == dim)
                {
                    (*dimension->mutable_bindings())[std::to_string(key.second)] = name;
                }
            }
        }

        for(const auto & [position, running_instance] : event.static_ri)
        {
            RunningInstance instance;
            instance.set_start_point(running_instance.first);
            instance.set_transformation_shift(running_instance.second);
            (*connector->mutable_static_ri())[position] = instance;
        }

        return record;
    }
}

namespace dcn::parse
{
    namespace
    {
        std::optional<std::int32_t> _parseJsonInt32(const json & json_value)
        {
            if(json_value.is_number_unsigned())
            {
                const auto value = json_value.get<json::number_unsigned_t>();
                if(value > static_cast<json::number_unsigned_t>(std::numeric_limits<std::int32_t>::max()))
                {
                    return std::nullopt;
                }

                return static_cast<std::int32_t>(value);
            }

            if(json_value.is_number_integer())
            {
                const auto value = json_value.get<json::number_integer_t>();
                if(value < static_cast<json::number_integer_t>(std::numeric_limits<std::int32_t>::min()) ||
                   value > static_cast<json::number_integer_t>(std::numeric_limits<std::int32_t>::max()))
                {
                    return std::nullopt;
                }

                return static_cast<std::int32_t>(value);
            }

            return std::nullopt;
        }

        std::optional<ParseError> _validateDimensionSemantics(const Dimension & dimension)
        {
            if(dimension.composite().empty() && dimension.bindings_size() != 0)
            {
                return ParseError{
                    ParseError::Kind::INVALID_VALUE,
                    "bindings are not allowed for scalar dimensions"};
            }

            for(const auto & [slot, _] : dimension.bindings())
            {
                const auto parsed_slot = parseUint32Decimal(slot);
                if(!parsed_slot || slot != std::to_string(*parsed_slot))
                {
                    return ParseError{
                        ParseError::Kind::INVALID_VALUE,
                        std::format("invalid canonical binding slot key `{}`", slot)};
                }
            }

            return std::nullopt;
        }

        std::optional<ParseError> _validateConnectorSemantics(const Connector & connector)
        {
            if(connector.dimensions_size() <= 0)
            {
                return ParseError{
                    ParseError::Kind::INVALID_VALUE,
                    "connector must define at least one dimension"};
            }

            for(const Dimension & dimension : connector.dimensions())
            {
                if(const auto dimension_error = _validateDimensionSemantics(dimension); dimension_error)
                {
                    return *dimension_error;
                }
            }

            return std::nullopt;
        }
    }

    template<>
    Result<json> parseToJson(TransformationDef transform_def, use_json_t)
    {
        json json_obj = json::object();
        json_obj["name"] = transform_def.name();
        json_obj["args"] = transform_def.args();
        return json_obj;
    }

    template<>
    Result<TransformationDef> parseFromJson(json json_obj, use_json_t)
    {
        TransformationDef transform_def;

        if(json_obj.contains("name") == false || !json_obj["name"].is_string())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "name not found"});
        }
        transform_def.set_name(json_obj["name"].get<std::string>());

        if(json_obj.contains("args"))
        {
            if(!json_obj["args"].is_array())
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid args"});
            }
            for(const auto & arg : json_obj["args"])
            {
                const auto parsed_arg = _parseJsonInt32(arg);
                if(!parsed_arg.has_value())
                {
                    return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid args"});
                }

                transform_def.add_args(*parsed_arg);
            }
        }

        return transform_def;
    }

    template<>
    Result<json> parseToJson(Dimension dimension, use_json_t)
    {
        json json_obj = json::object();
        json_obj["composite"] = dimension.composite();
        json_obj["bindings"] = json::object();
        json_obj["transformations"] = json::array();

        for(const auto & [slot, composite] : dimension.bindings())
        {
            json_obj["bindings"][slot] = composite;
        }

        for(const TransformationDef & transform_def : dimension.transformations())
        {
            auto transform_result = parseToJson(transform_def, use_json);
            if(!transform_result)
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid transformation"});
            }

            json_obj["transformations"].emplace_back(std::move(*transform_result));
        }

        return json_obj;
    }

    template<>
    Result<Dimension> parseFromJson(json json_obj, use_json_t)
    {
        Dimension dimension;

        if(json_obj.contains("composite"))
        {
            if(!json_obj["composite"].is_string())
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid composite"});
            }
            dimension.set_composite(json_obj["composite"].get<std::string>());
        }
        else
        {
            dimension.set_composite("");
        }

        if(json_obj.contains("slot_bindings"))
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "slot_bindings is removed; use bindings"});
        }

        if(json_obj.contains("bindings"))
        {
            if(!json_obj["bindings"].is_object())
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid bindings"});
            }

            for(const auto & [slot, binding_json] : json_obj["bindings"].items())
            {
                if(!binding_json.is_string())
                {
                    return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid binding composite"});
                }

                (*dimension.mutable_bindings())[slot] = binding_json.get<std::string>();
            }
        }

        if(json_obj.contains("transformations") == false || !json_obj["transformations"].is_array())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "transformations not found"});
        }

        for(const auto & transformation_json : json_obj["transformations"])
        {
            auto transformation_def = parseFromJson<TransformationDef>(transformation_json, use_json);
            if(!transformation_def)
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid transformation"});
            }

            dimension.add_transformations();
            *dimension.mutable_transformations(dimension.transformations_size() - 1) = *transformation_def;
        }

        if(const auto dimension_error = _validateDimensionSemantics(dimension); dimension_error)
        {
            return std::unexpected(*dimension_error);
        }

        return dimension;
    }

    template<>
    Result<std::string> parseToJson(Dimension dimension, use_protobuf_t)
    {
        google::protobuf::util::JsonPrintOptions options;
        options.add_whitespace = true;
        options.preserve_proto_field_names = true;
        options.always_print_fields_with_no_presence = true;

        std::string json_output;
        auto status = google::protobuf::util::MessageToJsonString(dimension, &json_output, options);

        if(!status.ok())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid dimension"});
        }

        return json_output;
    }

    template<>
    Result<Dimension> parseFromJson(std::string json_str, use_protobuf_t)
    {
        google::protobuf::util::JsonParseOptions options;

        Dimension dimension;
        auto status = google::protobuf::util::JsonStringToMessage(json_str, &dimension, options);

        if(!status.ok())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid dimension"});
        }

        if(const auto dimension_error = _validateDimensionSemantics(dimension); dimension_error)
        {
            return std::unexpected(*dimension_error);
        }

        return dimension;
    }

    template<>
    Result<json> parseToJson(Connector connector, use_json_t)
    {
        json json_obj = json::object();
        json_obj["name"] = connector.name();

        json_obj["dimensions"] = json::array();
        for(const Dimension & dimension : connector.dimensions())
        {
            auto dimension_result = parseToJson(dimension, use_json);
            if(!dimension_result)
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid dimension"});
            }
            json_obj["dimensions"].emplace_back(std::move(*dimension_result));
        }

        json_obj["condition_name"] = connector.condition_name();
        json_obj["condition_args"] = json::array();
        for(const auto & condition_arg : connector.condition_args())
        {
            json_obj["condition_args"].emplace_back(condition_arg);
        }

        json_obj["static_ri"] = json::object();
        std::vector<std::pair<std::uint32_t, RunningInstance>> sorted_static_ri;
        sorted_static_ri.reserve(static_cast<std::size_t>(connector.static_ri_size()));
        for(const auto & [position, running_instance] : connector.static_ri())
        {
            sorted_static_ri.emplace_back(position, running_instance);
        }
        std::ranges::sort(sorted_static_ri, [](const auto & lhs, const auto & rhs)
        {
            return lhs.first < rhs.first;
        });

        for(const auto & [position, running_instance] : sorted_static_ri)
        {
            auto running_instance_res = parseToJson(running_instance, use_json);
            if(!running_instance_res)
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid static_ri value"});
            }

            json_obj["static_ri"][std::to_string(position)] = std::move(*running_instance_res);
        }

        return json_obj;
    }

    template<>
    Result<Connector> parseFromJson(json json_obj, use_json_t)
    {
        Connector connector;

        if(json_obj.contains("name") == false || !json_obj["name"].is_string())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid name"});
        }
        connector.set_name(json_obj["name"].get<std::string>());

        if(json_obj.contains("feature_name"))
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "feature_name is removed; use dimensions"});
        }

        if(json_obj.contains("dimensions") == false || !json_obj["dimensions"].is_array())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid dimensions"});
        }
        if(json_obj["dimensions"].empty())
        {
            return std::unexpected(ParseError{
                ParseError::Kind::INVALID_VALUE,
                "connector must define at least one dimension"});
        }

        for(const auto & dimension_json : json_obj["dimensions"])
        {
            auto dimension = parseFromJson<Dimension>(dimension_json, use_json);
            if(!dimension)
            {
                return std::unexpected(dimension.error());
            }

            connector.add_dimensions();
            *connector.mutable_dimensions(connector.dimensions_size() - 1) = *dimension;
        }

        if(json_obj.contains("composite_names"))
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "composite_names is removed; use dimensions[].composite"});
        }

        if(json_obj.contains("composites"))
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "composites is removed; use dimensions[].composite"});
        }

        if(json_obj.contains("condition_name") == false || !json_obj["condition_name"].is_string())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition_name"});
        }
        connector.set_condition_name(json_obj["condition_name"].get<std::string>());

        if(json_obj.contains("condition_args") == false || !json_obj["condition_args"].is_array())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition_args"});
        }
        for(const auto & condition_arg_json : json_obj["condition_args"])
        {
            const auto parsed_arg = _parseJsonInt32(condition_arg_json);
            if(!parsed_arg.has_value())
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition_args"});
            }

            connector.add_condition_args(*parsed_arg);
        }

        if(json_obj.contains("static_ri"))
        {
            if(!json_obj["static_ri"].is_object())
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid static_ri"});
            }

            for(const auto & [position_key, running_instance_json] : json_obj["static_ri"].items())
            {
                const auto position_opt = parse::parseUint32Decimal(position_key);
                if(!position_opt || position_key != std::to_string(*position_opt))
                {
                    return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid static_ri key"});
                }

                auto running_instance_res = parseFromJson<RunningInstance>(running_instance_json, use_json);
                if(!running_instance_res)
                {
                    return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid static_ri value"});
                }

                (*connector.mutable_static_ri())[*position_opt] = *running_instance_res;
            }
        }

        if(const auto connector_error = _validateConnectorSemantics(connector); connector_error)
        {
            return std::unexpected(*connector_error);
        }

        return connector;
    }

    template<>
    Result<std::string> parseToJson(Connector connector, use_protobuf_t)
    {
        google::protobuf::util::JsonPrintOptions options;
        options.add_whitespace = true;
        options.preserve_proto_field_names = true;
        options.always_print_fields_with_no_presence = true;

        std::string json_str;
        auto status = google::protobuf::util::MessageToJsonString(connector, &json_str, options);

        if(!status.ok())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid connector"});
        }

        return json_str;
    }

    template<>
    Result<Connector> parseFromJson(std::string json_str, use_protobuf_t)
    {
        google::protobuf::util::JsonParseOptions options;

        Connector connector;
        auto status = google::protobuf::util::JsonStringToMessage(json_str, &connector, options);

        if(!status.ok())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid connector"});
        }

        if(const auto connector_error = _validateConnectorSemantics(connector); connector_error)
        {
            return std::unexpected(*connector_error);
        }

        return connector;
    }

    template<>
    Result<json> parseToJson(ConnectorRecord connector_record, use_json_t)
    {
        json json_obj = json::object();

        auto connector_result = parseToJson(connector_record.connector(), use_json);
        if(!connector_result)
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid connector"});
        }

        json_obj["connector"] = std::move(*connector_result);
        json_obj["owner"] = connector_record.owner();
        return json_obj;
    }

    template<>
    Result<std::string> parseToJson(ConnectorRecord connector_record, use_protobuf_t)
    {
        google::protobuf::util::JsonPrintOptions options;
        options.add_whitespace = true;
        options.preserve_proto_field_names = true;
        options.always_print_fields_with_no_presence = true;

        std::string json_str;
        auto status = google::protobuf::util::MessageToJsonString(connector_record, &json_str, options);

        if(!status.ok())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid connector record"});
        }

        return json_str;
    }

    template<>
    Result<ConnectorRecord> parseFromJson(json json_obj, use_json_t)
    {
        ConnectorRecord connector_record;
        if(json_obj.contains("connector") == false)
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid connector"});
        }

        auto connector = parseFromJson<Connector>(json_obj["connector"], use_json);
        if(!connector)
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid connector"});
        }
        *connector_record.mutable_connector() = std::move(*connector);

        if(json_obj.contains("owner") == false)
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid owner"});
        }
        connector_record.set_owner(json_obj["owner"].get<std::string>());

        return connector_record;
    }

    template<>
    Result<ConnectorRecord> parseFromJson(std::string json_str, use_protobuf_t)
    {
        google::protobuf::util::JsonParseOptions options;
        options.ignore_unknown_fields = true;

        ConnectorRecord connector_record;
        auto status = google::protobuf::util::JsonStringToMessage(json_str, &connector_record, options);

        if(!status.ok())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid connector record"});
        }

        return connector_record;
    }
}
