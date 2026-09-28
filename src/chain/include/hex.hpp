#pragma once

#include <cstdint>
#include <span>
#include <string>

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

    std::string toHexQuantity(const std::int64_t value);

    // 0x-prefixed lowercase hex of arbitrary bytes.
    std::string bytesToHex(std::span<const std::uint8_t> bytes);
}

namespace dcn::parse
{
    Result<std::int64_t> parseHexQuantity(const std::string & value);
}
