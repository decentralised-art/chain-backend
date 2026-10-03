#include "parser.hpp"

#include <charconv>
#include <format>

namespace dcn::parse
{
    namespace
    {
        template<class T>
        Result<T> parseUnsignedDecimal(std::string_view value, std::string_view type_name)
        {
            T parsed = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
            if(ec == std::errc::result_out_of_range)
            {
                return std::unexpected(ParseError{
                    .kind = ParseError::Kind::OUT_OF_RANGE,
                    .message = std::format("{} decimal value is out of range", type_name)});
            }

            if(ec != std::errc{} || ptr != (value.data() + value.size()))
            {
                return std::unexpected(ParseError{
                    .kind = ParseError::Kind::INVALID_VALUE,
                    .message = std::format("invalid {} decimal value", type_name)});
            }

            return parsed;
        }
    }

    std::string dumpJsonLossy(const json & value)
    {
        return value.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    Result<std::uint32_t> parseUint32Decimal(std::string_view value)
    {
        return parseUnsignedDecimal<std::uint32_t>(value, "uint32");
    }

    Result<std::uint64_t> parseUint64Decimal(std::string_view value)
    {
        return parseUnsignedDecimal<std::uint64_t>(value, "uint64");
    }
}
