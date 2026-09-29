#pragma once

#include <format>

namespace dcn::http
{
    /**
     * @brief Enum to represent HTTP response status codes.
     * 
     * This enum represents the possible status codes of an HTTP response.
     * 
     * The values of the enum are categorized as follows:
     * 
     * - `Unknown`: The response code is not recognized or not supported.
     * 
     * - Level 200: Successful responses.
     * 
     *     - - `OK`: The request succeeded.
     * 
     * - Level 400: Client error responses.
     * 
     *     - - `BadRequest`: The request could not be understood by the server.
     * 
     *     - - `Unauthorized`: The request requires user authentication.
     * 
     *     - - `Forbidden`: The server understood the request, but refuses to authorize it.
     * 
     *     - - `NotFound`: The server has not found anything matching the request.
     * 
     *     - - `PayloadTooLarge`: The request body is larger than the server accepts.
     * 
     *     - - `RequestHeaderFieldsTooLarge`: The request line and headers are larger than the server accepts.
     * 
     * - Level 500: Server error responses.
     * 
     *     - - `InternalServerError`: The server encountered an unexpected condition.
     * 
     *     - - `ServiceUnavailable`: The server is currently unable to handle the request.
     * 
     *     - - `GatewayTimeout`: The server did not receive a timely response from an upstream server.
     */
    enum class Code
    {
        // Unknown, specially reserved
        Unknown = 0,

        // level 200
        OK = 200,
        Created = 201,
        Accepted = 202,
        NoContent = 204,

        // LEVEL 400
        BadRequest = 400,
        Unauthorized = 401,
        Forbidden = 403,
        NotFound = 404,
        Conflict = 409,
        PayloadTooLarge = 413,
        RequestHeaderFieldsTooLarge = 431,

        // LEVEL 500
        InternalServerError = 500,
        BadGateway = 502,
        ServiceUnavailable = 503
    };
}

template <>
struct std::formatter<dcn::http::Code> : std::formatter<std::string> {
  auto format(dcn::http::Code code, format_context& ctx) const {
    switch(code)
    {
        // level 200
        case dcn::http::Code::OK:                  return formatter<string>::format("200 OK", ctx);
        case dcn::http::Code::Created:             return formatter<string>::format("201 Created", ctx);
        case dcn::http::Code::Accepted:            return formatter<string>::format("202 Accepted", ctx);
        case dcn::http::Code::NoContent:           return formatter<string>::format("204 No Content", ctx);

        // LEVEL 400
        case dcn::http::Code::BadRequest:          return formatter<string>::format("400 Bad Request", ctx);
        case dcn::http::Code::Unauthorized:        return formatter<string>::format("401 Unauthorized", ctx);
        case dcn::http::Code::Forbidden:           return formatter<string>::format("403 Forbidden", ctx);
        case dcn::http::Code::NotFound:            return formatter<string>::format("404 Not Found", ctx);
        case dcn::http::Code::Conflict:            return formatter<string>::format("409 Conflict", ctx);
        case dcn::http::Code::PayloadTooLarge:     return formatter<string>::format("413 Payload Too Large", ctx);
        case dcn::http::Code::RequestHeaderFieldsTooLarge:
                                                   return formatter<string>::format("431 Request Header Fields Too Large", ctx);
        
        // LEVEL 500
        case dcn::http::Code::InternalServerError: return formatter<string>::format("500 Internal Server Error", ctx);
        case dcn::http::Code::BadGateway:          return formatter<string>::format("502 Bad Gateway", ctx);
        case dcn::http::Code::ServiceUnavailable:  return formatter<string>::format("503 Service Unavailable", ctx);

        // Unknown
        case dcn::http::Code::Unknown:      return formatter<string>::format("Unknown", ctx);
    }
    return formatter<string>::format("", ctx);
  }
};
