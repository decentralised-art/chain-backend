#pragma once

#include <cstdint>
#include <format>
#include <vector>

#include "chain.hpp"
#include "parser.hpp"
#include "crypto.hpp"


namespace dcn::pt
{
    struct PTDeployError 
    {
        enum class Kind : std::uint8_t
        {
            UNKNOWN = 0,
            
            INVALID_INPUT,

            CONNECTOR_ALREADY_REGISTERED,
            CONNECTOR_MISSING,
            CONNECTOR_DIMENSIONS_MISMATCH,

            TRANSFORMATION_ALREADY_REGISTERED,
            TRANSFORMATION_ARGUMENTS_MISMATCH,
            TRANSFORMATION_MISSING,

            CONDITION_ALREADY_REGISTERED,
            CONDITION_ARGUMENTS_MISMATCH,
            CONDITION_MISSING,

            REGISTRY_ERROR,

        } kind = Kind::UNKNOWN;

        evmc_bytes32 a{};  // first bytes32 (or zero)
        uint32_t code{};   // for RegistryError
    };

    struct PTExecuteError
    {
        enum class Kind : std::uint8_t
        {
            UNKNOWN = 0,
            CONDITION_NOT_MET,
            RUNNING_INSTANCE_DUPLICATE,
            RUNNING_INSTANCE_NOT_SORTED,
            RUNNING_INSTANCE_STATIC_OVERRIDE

        } kind = Kind::UNKNOWN;

        evmc_bytes32 a{};  // first bytes32 (or zero)
    };

}

namespace dcn::parse
{
    template<>
    Result<pt::PTDeployError> decodeBytes(const std::vector<uint8_t> & bytes);

    template<>
    Result<pt::PTExecuteError> decodeBytes(const std::vector<uint8_t> & bytes);
}

template <>
struct std::formatter<dcn::pt::PTDeployError::Kind> : std::formatter<std::string> {
    auto format(const dcn::pt::PTDeployError::Kind & err, format_context& ctx) const {
        switch(err)
        {
            case dcn::pt::PTDeployError::Kind::INVALID_INPUT : return formatter<string>::format("Invalid input", ctx);

            case dcn::pt::PTDeployError::Kind::CONNECTOR_ALREADY_REGISTERED : return formatter<string>::format("Connector already registered", ctx);
            case dcn::pt::PTDeployError::Kind::CONNECTOR_MISSING : return formatter<string>::format("Connector missing", ctx);
            case dcn::pt::PTDeployError::Kind::CONNECTOR_DIMENSIONS_MISMATCH : return formatter<string>::format("Connector dimensions mismatch", ctx);

            case dcn::pt::PTDeployError::Kind::TRANSFORMATION_ALREADY_REGISTERED : return formatter<string>::format("Transformation already registered", ctx);
            case dcn::pt::PTDeployError::Kind::TRANSFORMATION_ARGUMENTS_MISMATCH : return formatter<string>::format("Transformation arguments mismatch", ctx);
            case dcn::pt::PTDeployError::Kind::TRANSFORMATION_MISSING : return formatter<string>::format("Transformation missing", ctx);
            
            case dcn::pt::PTDeployError::Kind::CONDITION_ALREADY_REGISTERED : return formatter<string>::format("Condition already registered", ctx);
            case dcn::pt::PTDeployError::Kind::CONDITION_ARGUMENTS_MISMATCH : return formatter<string>::format("Condition arguments mismatch", ctx);
            case dcn::pt::PTDeployError::Kind::CONDITION_MISSING : return formatter<string>::format("Condition missing", ctx);
            case dcn::pt::PTDeployError::Kind::REGISTRY_ERROR : return formatter<string>::format("Registry error", ctx);

            case dcn::pt::PTDeployError::Kind::UNKNOWN : return formatter<string>::format("Unknown", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};

template <>
struct std::formatter<dcn::pt::PTExecuteError::Kind> : std::formatter<std::string> {
    auto format(const dcn::pt::PTExecuteError::Kind & err, format_context& ctx) const {
        switch(err)
        {
            case dcn::pt::PTExecuteError::Kind::CONDITION_NOT_MET : return formatter<string>::format("Condition not met", ctx);
            case dcn::pt::PTExecuteError::Kind::RUNNING_INSTANCE_DUPLICATE : return formatter<string>::format("Running instance duplicate", ctx);
            case dcn::pt::PTExecuteError::Kind::RUNNING_INSTANCE_NOT_SORTED : return formatter<string>::format("Running instance not sorted", ctx);
            case dcn::pt::PTExecuteError::Kind::RUNNING_INSTANCE_STATIC_OVERRIDE : return formatter<string>::format("Running instance static override", ctx);

            case dcn::pt::PTExecuteError::Kind::UNKNOWN : return formatter<string>::format("Unknown", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};
