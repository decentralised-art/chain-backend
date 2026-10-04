#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "byte_order.hpp"
#include "keccak256.hpp"

namespace dcn::crypto
{
    constexpr std::size_t HMAC_SHA256_LEN = 32;

    /**
     * @brief `size` bytes from OpenSSL's CSPRNG, which the operating system seeds.
     * @throws std::runtime_error when the generator cannot produce them.
     */
    std::string randomBytes(std::size_t size);

    /// HMAC-SHA256 (RFC 2104) of `data` under `key`.
    std::array<std::uint8_t, HMAC_SHA256_LEN> hmacSha256(std::string_view key, std::string_view data);

    /// Whether `a` and `b` are equal, in time that depends only on their sizes.
    bool equalConstantTime(std::string_view a, std::string_view b);
}
