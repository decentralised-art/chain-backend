#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <vector>

#ifdef interface
    #undef interface
#endif
#include <evmc/evmc.hpp>
#ifndef interface
    #define interface __STRUCT__
#endif

namespace dcn::chain
{
    struct DeployError
    {
        enum class Kind : std::uint8_t
        {
            UNKNOWN = 0,
            INVALID_INPUT,
            TRANSACTION_REVERTED
        } kind = Kind::UNKNOWN;

        std::string message{};
        std::vector<std::uint8_t> result_bytes{};
    };
}

template <>
struct std::formatter<dcn::chain::DeployError::Kind> : std::formatter<std::string>
{
    auto format(const dcn::chain::DeployError::Kind & err, format_context & ctx) const
    {
        switch(err)
        {
            case dcn::chain::DeployError::Kind::INVALID_INPUT:
                return formatter<string>::format("Invalid input", ctx);
            case dcn::chain::DeployError::Kind::TRANSACTION_REVERTED:
                return formatter<string>::format("Transaction reverted", ctx);
            case dcn::chain::DeployError::Kind::UNKNOWN:
                return formatter<string>::format("Unknown", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};
