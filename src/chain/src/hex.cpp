#include "hex.hpp"
#include "utils.hpp"

namespace dcn::chain
{
    std::string bytes32ToHex(const evmc::bytes32 & value)
    {
        return normalizeHex(evmc::hex(value));
    }


    std::string withHexPrefix(std::string value)
    {
        if(value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
        {
            return value;
        }
        return std::string("0x") + value;
    }

    std::string normalizeHex(std::string value)
    {
        return utils::toLower(withHexPrefix(std::move(value)));
    }

    std::string toHexQuantity(const std::int64_t value)
    {
        if(value < 0)
        {
            return "0x0";
        }
        return std::format("0x{:x}", static_cast<std::uint64_t>(value));
    }

    std::string bytesToHex(const std::span<const std::uint8_t> bytes)
    {
        return std::string("0x") + evmc::hex(evmc::bytes_view(bytes.data(), bytes.size()));
    }
}

namespace dcn::parse
{
    Result<std::int64_t> parseHexQuantity(const std::string & value)
    {
        try
        {
            if(value.empty())
            {
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
            }

            std::string stripped = value;
            int base = 10;
            if(stripped.rfind("0x", 0) == 0 || stripped.rfind("0X", 0) == 0)
            {
                stripped = stripped.substr(2);
                base = 16;
            }

            if(stripped.empty())
            {
                return static_cast<std::int64_t>(0);
            }

            const unsigned long long parsed = std::stoull(stripped, nullptr, base);
            if(parsed > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max()))
            {
                return std::unexpected(ParseError{ParseError::Kind::OUT_OF_RANGE});
            }
            
            return static_cast<std::int64_t>(parsed);
        }
        catch(...)
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        }
    }
}