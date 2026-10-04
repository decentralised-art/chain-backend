#pragma once

#include <cstdint>
#include <string>
#include <expected>
#include <format>

namespace dcn::parse
{   
    struct ParseError
    {
        enum class Kind : std::uint8_t
        {
            UNKNOWN         = 0U,

            INVALID_VALUE   = 1U,
            OUT_OF_RANGE    = 2U,
            TYPE_MISMATCH   = 3U
        };
        
        Kind kind = Kind::UNKNOWN;
        std::string message = "";
    };

    template<class T>
    using Result = std::expected<T, ParseError>;
}

template <>
struct std::formatter<dcn::parse::ParseError::Kind> : std::formatter<std::string> {
    auto format(const dcn::parse::ParseError::Kind & err, format_context& ctx) const {
        switch(err)
        {
            case dcn::parse::ParseError::Kind::INVALID_VALUE : return formatter<string>::format("Invalid value", ctx);
            case dcn::parse::ParseError::Kind::OUT_OF_RANGE : return formatter<string>::format("Out of range", ctx);
            case dcn::parse::ParseError::Kind::TYPE_MISMATCH : return formatter<string>::format("Type mismatch", ctx);

            case dcn::parse::ParseError::Kind::UNKNOWN : return formatter<string>::format("Unknown", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};