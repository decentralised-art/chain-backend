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
    struct ExecuteError
    {
        enum class Kind : std::uint8_t
        {
            UNKNOWN = 0,
            INVALID_CONFIG,
            INVALID_INPUT,
            RPC_ERROR,
            RPC_MALFORMED,
            SIGNING_ERROR,
            TRANSACTION_REVERTED,
            TIMEOUT
        } kind = Kind::UNKNOWN;

        std::string message{};
        std::vector<std::uint8_t> result_bytes{};
    };

    struct ExecuteReceipt
    {

    };
}

template <>
struct std::formatter<dcn::chain::ExecuteError::Kind> : std::formatter<std::string>
{
    auto format(const dcn::chain::ExecuteError::Kind & err, format_context & ctx) const
    {
        switch(err)
        {
            case dcn::chain::ExecuteError::Kind::INVALID_CONFIG:
                return formatter<string>::format("Invalid config", ctx);
            case dcn::chain::ExecuteError::Kind::INVALID_INPUT:
                return formatter<string>::format("Invalid input", ctx);
            case dcn::chain::ExecuteError::Kind::RPC_ERROR:
                return formatter<string>::format("RPC error", ctx);
            case dcn::chain::ExecuteError::Kind::RPC_MALFORMED:
                return formatter<string>::format("Malformed RPC response", ctx);
            case dcn::chain::ExecuteError::Kind::SIGNING_ERROR:
                return formatter<string>::format("Signing error", ctx);
            case dcn::chain::ExecuteError::Kind::TRANSACTION_REVERTED:
                return formatter<string>::format("Transaction reverted", ctx);
            case dcn::chain::ExecuteError::Kind::TIMEOUT:
                return formatter<string>::format("Timeout", ctx);
            case dcn::chain::ExecuteError::Kind::UNKNOWN:
                return formatter<string>::format("Unknown", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};
