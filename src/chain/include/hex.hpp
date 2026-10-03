#pragma once

#include <concepts>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <type_traits>

#ifdef interface
    #undef interface
#endif
#include <evmc/evmc.hpp>
#ifndef interface
    #define interface __STRUCT__
#endif

#include "parser.hpp"


namespace dcn::chain
{
    std::string bytes32ToHex(const evmc::bytes32 & value);

    std::string withHexPrefix(std::string value);

    std::string normalizeHex(std::string value);

    // JSON-RPC QUANTITY; negative values encode as 0x0.
    template<std::integral T>
    std::string toHexQuantity(const T value)
    {
        if constexpr(std::is_signed_v<T>)
        {
            if(value < 0) return "0x0";
        }
        return std::format("0x{:x}", static_cast<std::make_unsigned_t<T>>(value));
    }

    // Hex of exactly 32 bytes, with or without a 0x prefix.
    bool isBytes32Hex(const std::string & value);

    // 0x-prefixed lowercase hex of arbitrary bytes.
    std::string bytesToHex(std::span<const std::uint8_t> bytes);
}

namespace dcn::parse
{
    Result<std::int64_t> parseHexQuantity(const std::string & value);
}
