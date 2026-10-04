#pragma once
#include <string>
#include <algorithm>

#include <absl/hash/hash.h>
#include <spdlog/spdlog.h>

#include "condition.pb.h"

#include "parser.hpp"
#include "chain.hpp"

namespace dcn
{
    /**
     * @brief Combines hash values for a Condition object.
     *
     * @tparam H The hash state type.
     * @param h The initial hash state.
     * @param c The Condition object whose attributes will be hashed.
     * @return A combined hash state incorporating the chain-derivable identity (name and args_count).
     */
    template <typename H>
    inline H AbslHashValue(H h, const Condition & c) {
        return H::combine(std::move(h), c.name(), c.args_count());
    }

    /**
     * @brief Counts the number of distinct args[N] slots referenced by a condition's source.
     *        This is the chain-derivable args_count (it matches the value registered on chain).
     * @param sol_src The user-supplied Solidity source fragment.
     */
    parse::Result<std::uint32_t> countConditionArgs(const std::string & sol_src);

    parse::Result<std::string> constructConditionSolidityCode(const Condition & condition);
}

namespace dcn::pt
{
    struct ConditionAddedEvent
    {
        chain::Address caller{};
        std::string name;
        chain::Address condition_address{};
        chain::Address owner{};
        std::uint32_t args_count{};
        evmc::bytes32 content_hash{};
        evmc::bytes32 metadata_hash{};
        evmc::bytes32 runtime_code_hash{};
    };

    std::optional<ConditionAddedEvent> decodeConditionAddedEvent(
        const std::uint8_t* data,
        std::size_t data_size,
        const evmc::bytes32 topics[],
        std::size_t num_topics);

    std::optional<ConditionAddedEvent> decodeConditionAddedEvent(
        const std::string & data_hex,
        const std::vector<std::string> & topics_hex);

}

namespace dcn::parse
{
    /**
     * @brief Parses a condition object to a JSON object.
     * @param condition The condition object to parse.
     */
    template<>
    Result<json> parseToJson(Condition condition, use_json_t);

    /**
     * @brief Parses a JSON object to a condition object.
     * @param json_obj The JSON object to parse.
     */
    template<>
    Result<Condition> parseFromJson(json json_obj, use_json_t);

    /**
     * @brief Converts a condition object to a JSON string using protobuf
     * @param condition The condition object to convert
     * @return A JSON string representation of the condition object
     */
    template<>
    Result<std::string> parseToJson(Condition condition, use_protobuf_t);

    /**
     * @brief Parses a JSON string to a condition object using Protobuf.
     * @param json_str The JSON string to parse.
     */
    template<>
    Result<Condition> parseFromJson(std::string json_str, use_protobuf_t);


    /**
     * @brief Converts a ConditionRecord object to a JSON object.
     * @param condition_record The ConditionRecord object to convert.
     */
    template<>
    Result<json> parseToJson(ConditionRecord condition_record, use_json_t);

    /**
     * @brief Converts a ConditionRecord object to a JSON string using Protobuf.
     * @param condition_record The ConditionRecord object to convert.
     */
    template<>
    Result<std::string> parseToJson(ConditionRecord condition_record, use_protobuf_t);

    /**
     * @brief Parses a JSON object to a ConditionRecord object.
     * @param json_obj The JSON object to parse.
     */
    template<>
    Result<ConditionRecord> parseFromJson(json json_obj, use_json_t);
    
    /**
     * @brief Parses a JSON string to a ConditionRecord object using Protobuf.
     * @param json_str The JSON string to parse.
     */
    template<>
    Result<ConditionRecord> parseFromJson(std::string json_str, use_protobuf_t);
}
