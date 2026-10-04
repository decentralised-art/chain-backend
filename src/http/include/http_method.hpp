#pragma once

#include <format>
#include <string>

namespace dcn::http
{
    /**
     * @brief Enum to represent the request method.
     * 
     * This enum represents the possible methods of an HTTP request.
     * 
     * The values of the enum are:
     * 
     * - `Unknown`: The request method is not recognized or not supported.
     * 
     * - `GET`: method requests a representation of the specified resource.
     * 
     * - `HEAD`: method requests headers of the response, not the actual body.
     *
     * - `PUT`: replaces all current representations of the target resource with the request payload.
     * 
     * - `DEL`: Tdeletes the specified resource.
     * 
     * - `POST`: sends data to the server to create a new resource.
     * 
     * - `OPTIONS`: requests information about the communication options for the target resource.
     * 
     */
    enum class Method
    {
        //Unknown, specially reserved
        Unknown = 0,

        GET,
        HEAD,
        PUT,
        DEL, //DELETE collides with macro definition in winnt.h ... 
        POST,
        OPTIONS
    };
}

namespace dcn::parse
{
    /**
     * @brief Parse the given string to a `http::Method`.
     * 
     * @param method The string to be parsed.
     * 
     * @return The parsed `Method` or `Method::Unknown` if the string doesn't match any of the methods.
     */
    http::Method parseMethodFromString(const std::string & method);
}

template <>
struct std::formatter<dcn::http::Method> : std::formatter<std::string> {

  auto format(dcn::http::Method method, format_context& ctx) const {
    switch(method)
    {
        case dcn::http::Method::GET:     return formatter<string>::format("GET", ctx);
        case dcn::http::Method::HEAD:     return formatter<string>::format("HEAD", ctx);
        case dcn::http::Method::PUT:     return formatter<string>::format("PUT", ctx);
        case dcn::http::Method::DEL:     return formatter<string>::format("DELETE", ctx);
        case dcn::http::Method::POST:    return formatter<string>::format("POST", ctx);
        case dcn::http::Method::OPTIONS:    return formatter<string>::format("OPTIONS", ctx);

        // Unknown
        case dcn::http::Method::Unknown:    return formatter<string>::format("Unknown", ctx);
    }
    return formatter<string>::format("Unknown", ctx);
  }
};