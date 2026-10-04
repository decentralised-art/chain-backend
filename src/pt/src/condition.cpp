#include <cstring>
#include <format>

#include "crypto.hpp"

#include "condition.hpp"
#include "solidity_identifier.hpp"

namespace dcn
{
    parse::Result<std::uint32_t> countConditionArgs(const std::string & sol_src)
    {
        std::regex used_args_pattern(R"(args\[(\d+)\])");
        std::uint32_t argc = 0;

        std::smatch match;
        auto it = sol_src.cbegin();
        while (std::regex_search(it, sol_src.cend(), match, used_args_pattern))
        {
            try
            {
                unsigned long value = std::stoul(match[1].str());

                if (value >= std::numeric_limits<std::uint32_t>::max()) {
                    spdlog::error("Condition args index overflows uint32_t argc computation");
                    return std::unexpected(parse::ParseError{
                        parse::ParseError::Kind::INVALID_VALUE,
                        "Condition args index must be less than uint32_t max"});
                }
                argc = std::max(argc, static_cast<std::uint32_t>(value) + 1U);
            }
            catch(const std::exception& e)
            {
                spdlog::error("Invalid argument index: {}", match[1].str());
                return std::unexpected(parse::ParseError{
                    parse::ParseError::Kind::INVALID_VALUE,
                    std::format("Invalid condition args index `{}`", match[1].str())});
            }

            it = match.suffix().first;
        }

        return argc;
    }

    parse::Result<std::string> constructConditionSolidityCode(const Condition & condition)
    {
        /* ------------- EXAMPLE -------------
        // SPDX-License-Identifier: GPL-3.0

        pragma solidity >=0.8.2 <0.9.0;

        import "../ConditionBase.sol";

        contract ConditionA is ConditionBase
        {
            constructor(bytes32 contentHash_, bytes32 metadataHash_)
                ConditionBase("ConditionA", 1, contentHash_, metadataHash_)
            {}

            function check(int32 [] calldata args) view external returns (bool)
            {
                require(args.length == this.getArgsCount(), "wrong number of arguments");
                return x + args[0];
            }
        }
        */

        if(!pt::isValidSolidityIdentifier(condition.name()))
        {
            spdlog::error("Condition `{}` has invalid Solidity identifier name", condition.name());
            return std::unexpected(parse::ParseError{
                parse::ParseError::Kind::INVALID_VALUE,
                std::format("Condition `{}` has invalid Solidity identifier name", condition.name())});
        }

        const auto argc_res = countConditionArgs(condition.sol_src());
        if(!argc_res)
        {
            return std::unexpected(argc_res.error());
        }
        const std::uint32_t argc = *argc_res;

        return  "//SPDX-License-Identifier: MIT\n"
                "pragma solidity >=0.8.2 <0.9.0;\n"
                "import \"condition/ConditionBase.sol\";\n"
                "contract " + condition.name() + " is ConditionBase{\n" // open contract
                "constructor(bytes32 contentHash_, bytes32 metadataHash_) ConditionBase(\""
                + condition.name() + "\"," + std::to_string(argc) + ", contentHash_, metadataHash_){}\n"
                "function check(int32 [] calldata args) view external returns (bool){\n" // open function
                "require(args.length == this.getArgsCount(), \"wrong number of arguments\");\n"
                + condition.sol_src() + "\n}" // close function
                "\n}"; // close contract
    }
}

namespace dcn::pt
{
    std::optional<ConditionAddedEvent> decodeConditionAddedEvent(
        const std::uint8_t* data,
        std::size_t data_size,
        const evmc::bytes32 topics[],
        std::size_t num_topics)
    {
        if(data == nullptr || topics == nullptr || num_topics < 1 || data_size < 32 * 8)
        {
            return std::nullopt;
        }

        const evmc::bytes32 expected_topic = chain::constructEventTopic(
            "ConditionAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)");

        if(topics[0] != expected_topic)
        {
            return std::nullopt;
        }

        const auto caller = chain::readAddressWord(data, data_size, 0);
        const auto name_offset = chain::readWordAsSizeT(data, data_size, 32);
        const auto condition_address = chain::readAddressWord(data, data_size, 64);
        const auto owner = chain::readAddressWord(data, data_size, 96);
        const auto args_count = chain::readUint32Word(data, data_size, 128);
        if(!caller || !name_offset || !condition_address || !owner || !args_count)
        {
            return std::nullopt;
        }

        evmc::bytes32 content_hash{};
        evmc::bytes32 metadata_hash{};
        evmc::bytes32 runtime_code_hash{};
        std::memcpy(content_hash.bytes, data + 160, 32);
        std::memcpy(metadata_hash.bytes, data + 192, 32);
        std::memcpy(runtime_code_hash.bytes, data + 224, 32);

        const auto name = chain::decodeAbiString(data, data_size, *name_offset);
        if(!name)
        {
            return std::nullopt;
        }

        return ConditionAddedEvent{
            .caller = *caller,
            .name = *name,
            .condition_address = *condition_address,
            .owner = *owner,
            .args_count = *args_count,
            .content_hash = content_hash,
            .metadata_hash = metadata_hash,
            .runtime_code_hash = runtime_code_hash
        };
    }

    std::optional<ConditionAddedEvent> decodeConditionAddedEvent(
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

        return decodeConditionAddedEvent(
            data_bytes->data(),
            data_bytes->size(),
            topic_words->data(),
            topic_words->size());
    }
}

namespace dcn::parse
{
    template<>
    Result<json> parseToJson(Condition condition, use_json_t)
    {
        json json_obj;

        json_obj["name"] = condition.name();
        json_obj["args_count"] = condition.args_count();
        // Always present; null while the deployed runtime code is unknown.
        json_obj["runtime_code"] = condition.has_runtime_code() ? json(condition.runtime_code()) : json(nullptr);

        return json_obj;
    }

    template<>
    Result<Condition> parseFromJson(json json_obj, use_json_t)
    {
        Condition condition;

        if (json_obj.contains("name")) {
            condition.set_name(json_obj["name"].get<std::string>());
        }
        else return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid name"});
        

        // sol_src is an execution input, kept in the local definition of an entity created here.
        if (json_obj.contains("sol_src")) {
            condition.set_sol_src(json_obj["sol_src"].get<std::string>());
        }

        if (json_obj.contains("args_count")) {
            condition.set_args_count(json_obj["args_count"].get<std::uint32_t>());
        }

        return condition;
    }

    template<>
    Result<std::string> parseToJson(Condition condition, use_protobuf_t)
    {
        google::protobuf::util::JsonPrintOptions options;
        options.add_whitespace = true;
        options.preserve_proto_field_names = true; // Use snake_case from proto
        options.always_print_fields_with_no_presence = true;

        std::string json_str;
        auto status = google::protobuf::util::MessageToJsonString(condition, &json_str, options);

        if (!status.ok()) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition"});

        return json_str;
    }

    template<>
    Result<Condition> parseFromJson(std::string json_str, use_protobuf_t)
    {
        google::protobuf::util::JsonParseOptions options;

        Condition condition;

        auto status = google::protobuf::util::JsonStringToMessage(json_str, &condition, options);

        if(!status.ok()) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition"});

        return condition;
    }

    template<>
    Result<json> parseToJson(ConditionRecord condition_record, use_json_t)
    {
        json json_obj = json::object();

        auto condition_result = parseToJson(condition_record.condition(), use_json);
        if(!condition_result) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition"});

        json_obj["condition"] = std::move(*condition_result);
        json_obj["owner"] = condition_record.owner();
        return json_obj;
    }

    template<>
    Result<std::string> parseToJson(ConditionRecord condition_record, use_protobuf_t)
    {
        google::protobuf::util::JsonPrintOptions options;
        options.add_whitespace = true;
        options.preserve_proto_field_names = true; // Use snake_case from proto
        options.always_print_fields_with_no_presence = true;

        std::string json_str;
        auto status = google::protobuf::util::MessageToJsonString(condition_record, &json_str, options);

        if (!status.ok()) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition record"});

        return json_str;
    }

    template<>
    Result<ConditionRecord> parseFromJson(json json_obj, use_json_t)
    {
        ConditionRecord condition_record;
        if (json_obj.contains("condition") == false)
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition"});

        auto condition = parseFromJson<Condition>(json_obj["condition"], use_json);

        if(!condition)
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition"});

        *condition_record.mutable_condition() = std::move(*condition);

        if (json_obj.contains("owner") == false)
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid owner"});
        
        condition_record.set_owner(json_obj["owner"].get<std::string>());

        return condition_record;
    }

    template<>
    Result<ConditionRecord> parseFromJson(std::string json_str, use_protobuf_t)
    {
        google::protobuf::util::JsonParseOptions options;
        options.ignore_unknown_fields = true;

        ConditionRecord condition_record;

        auto status = google::protobuf::util::JsonStringToMessage(json_str, &condition_record, options);

        if(!status.ok()) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "invalid condition record"});

        return condition_record;
    }

}
