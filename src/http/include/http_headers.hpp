#pragma once

#include <algorithm>
#include <format>
#include <string>
#include <vector>
#include <string>
#include <cctype>

#include "utils.hpp"

namespace dcn::http
{
    /**
     * @brief Enum of HTTP headers as defined by the standard
     *      This enum contains the most commonly used HTTP headers.
     * @see https://en.wikipedia.org/wiki/List_of_HTTP_header_fields
     */
    enum class Header
    {
        // Unknown, specially reserved
        Unknown = 0,

        Accept,

        AccessControlAllowOrigin,
        AccessControlAllowMethods,
        AccessControlAllowHeaders,
        AccessControlMaxAge,

        Authorization,

        Connection,
        ContentEncoding,
        ContentLength,
        ContentType,

        CacheControl,
        
        Date,
        Expect,

        Origin
    };

    /**
     * @brief A list of headers
     *      A HeadersList is a vector of pairs, where the first element of the pair is a Header enum value and the second element is the value of the header.
     *      The index of the Header enum value is used as the key in the vector.
     */
    using HeadersList = std::vector<std::pair<Header, std::string>>;
}

namespace dcn::parse
{
    /**
     * @brief Parse a header string to a Header enum
     * @param[in] header_str The header string to parse
     * @return The parsed Header enum
     */
    http::Header parseHeaderFromString(const std::string & header_str);
}

template <>
struct std::formatter<dcn::http::Header> : std::formatter<std::string> {
  auto format(dcn::http::Header header, format_context& ctx) const {
    switch(header)
    {
        // A
        case dcn::http::Header::Accept:                         return formatter<string>::format("Accept", ctx);
        case dcn::http::Header::AccessControlAllowOrigin:       return formatter<string>::format("Access-Control-Allow-Origin", ctx);
        case dcn::http::Header::AccessControlAllowMethods:      return formatter<string>::format("Access-Control-Allow-Methods", ctx);
        case dcn::http::Header::AccessControlAllowHeaders:      return formatter<string>::format("Access-Control-Allow-Headers", ctx);
        case dcn::http::Header::AccessControlMaxAge:            return formatter<string>::format("Access-Control-Max-Age", ctx);
        case dcn::http::Header::Authorization:                  return formatter<string>::format("Authorization", ctx);

        // B

        // C
        case dcn::http::Header::Connection:         return formatter<string>::format("Connection", ctx);
        case dcn::http::Header::ContentEncoding:    return formatter<string>::format("Content-Encoding", ctx);
        case dcn::http::Header::ContentLength:      return formatter<string>::format("Content-Length", ctx);
        case dcn::http::Header::ContentType:        return formatter<string>::format("Content-Type", ctx);
        case dcn::http::Header::CacheControl:        return formatter<string>::format("Cache-Control", ctx);

        // D
        case dcn::http::Header::Date:   return formatter<string>::format("Date", ctx);

        // E
        case dcn::http::Header::Expect: return formatter<string>::format("Expect", ctx);

        // O
        case dcn::http::Header::Origin: return formatter<string>::format("Origin", ctx);

        // Unknown
        case dcn::http::Header::Unknown:    return formatter<string>::format("Unknown", ctx);
    }
    return formatter<string>::format("Unknown", ctx);
  }
};

template <>
struct std::formatter<dcn::http::HeadersList> : std::formatter<std::string> {
  auto format(dcn::http::HeadersList headers_list, format_context& ctx) const {
    std::string headers_str = "";
    for(const auto& [header, header_value] : headers_list)
    {
        headers_str += std::format("{}: {}\n", header, header_value);
    }
    return formatter<string>::format(headers_str, ctx);
  }
};