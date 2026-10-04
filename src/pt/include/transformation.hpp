#pragma once
#include <string>
#include <algorithm>

#include <absl/hash/hash.h>
#include <spdlog/spdlog.h>

#include "transformation.pb.h"

#include "parser.hpp"
#include "chain.hpp"

namespace dcn
{
    /**
     * @brief Combines hash values for a Transformation object.
     *
     * @tparam H The hash state type.
     * @param h The initial hash state.
     * @param t The Transformation object whose attributes will be hashed.
     * @return A combined hash state incorporating the chain-derivable identity (name and args_count).
     */
    template <typename H>
    inline H AbslHashValue(H h, const Transformation& t) {
        return H::combine(std::move(h), t.name(), t.args_count());
    }

    /**
     * @brief Counts the number of distinct args[N] slots referenced by a transformation's source.
     *        This is the chain-derivable args_count (it matches the value registered on chain).
     * @param sol_src The user-supplied Solidity source fragment.
     */
    parse::Result<std::uint32_t> countTransformationArgs(const std::string & sol_src);

    parse::Result<std::string> constructTransformationSolidityCode(const Transformation & transformation);
}

namespace dcn::pt
{
    struct TransformationAddedEvent
    {
        chain::Address caller{};
        std::string name;
        chain::Address transformation_address{};
        chain::Address owner{};
        std::uint32_t args_count{};
        evmc::bytes32 content_hash{};
        evmc::bytes32 metadata_hash{};
        evmc::bytes32 runtime_code_hash{};
    };

    std::optional<TransformationAddedEvent> decodeTransformationAddedEvent(
        const std::uint8_t* data,
        std::size_t data_size,
        const evmc::bytes32 topics[],
        std::size_t num_topics);

    std::optional<TransformationAddedEvent> decodeTransformationAddedEvent(
        const std::string & data_hex,
        const std::vector<std::string> & topics_hex);

}

namespace dcn::parse
{   
    /**
     * @brief Parses a Transformation object to a JSON object.
     * @param transformation The Transformation object to parse.
     */
    template<>
    Result<json> parseToJson(Transformation transformation, use_json_t);

    /**
     * @brief Parses a JSON object to a Transformation object.
     * @param json_obj The JSON object to parse.
     */
    template<>
    Result<Transformation> parseFromJson(json json_obj, use_json_t);

    /**
     * @brief Converts a Transformation object to a JSON string using protobuf
     * @param transformation The Transformation object to convert
     * @return A JSON string representation of the Transformation object
     */
    template<>
    Result<std::string> parseToJson(Transformation transformation, use_protobuf_t);

    /**
     * @brief Parses a JSON string to a Transformation object using Protobuf.
     * @param json_str The JSON string to parse.
     */
    template<>
    Result<Transformation> parseFromJson(std::string json_str, use_protobuf_t);

    
    /**
     * @brief Converts a TransformationRecord object to a JSON object.
     * @param transformation_record The TransformationRecord object to convert.
     */
    template<>
    Result<json> parseToJson(TransformationRecord transformation_record, use_json_t);

    /**
     * @brief Converts a TransformationRecord object to a JSON string using Protobuf.
     * @param transformation_record The TransformationRecord object to convert.
     */
    template<>
    Result<std::string> parseToJson(TransformationRecord transformation_record, use_protobuf_t);

    /**
     * @brief Parses a JSON object to a TransformationRecord object.
     * @param json_obj The JSON object to parse.
     */
    template<>
    Result<TransformationRecord> parseFromJson(json json_obj, use_json_t);
    
    /**
     * @brief Parses a JSON string to a TransformationRecord object using Protobuf.
     * @param json_str The JSON string to parse.
     */
    template<>
    Result<TransformationRecord> parseFromJson(std::string json_str, use_protobuf_t);

}
