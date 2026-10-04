#include "crypto.hpp"

#include <limits>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace dcn::crypto
{
    std::string randomBytes(const std::size_t size)
    {
        if(size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            throw std::invalid_argument("Too many random bytes requested");
        }
        std::string bytes(size, '\0');
        if(RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), static_cast<int>(size)) != 1)
        {
            throw std::runtime_error("The OpenSSL random generator failed");
        }
        return bytes;
    }

    std::array<std::uint8_t, HMAC_SHA256_LEN> hmacSha256(const std::string_view key, const std::string_view data)
    {
        std::array<std::uint8_t, HMAC_SHA256_LEN> mac{};
        unsigned int mac_len = 0;
        if(key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
            || HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                reinterpret_cast<const unsigned char *>(data.data()), data.size(), mac.data(), &mac_len) == nullptr
            || mac_len != mac.size())
        {
            throw std::runtime_error("HMAC-SHA256 failed");
        }
        return mac;
    }

    bool equalConstantTime(const std::string_view a, const std::string_view b)
    {
        return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }
}
