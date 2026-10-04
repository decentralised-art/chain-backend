#include "api.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace dcn
{
    static parse::Result<std::string> _getAccessTokenFromHeader(const http::Request & request)
    {
        const auto auth_res = request.getHeader(http::Header::Authorization);
        if(auth_res.size() != 1)
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE, "Expected one Authorization header"});
        }

        // RFC 6750: the case-insensitive `Bearer` scheme, whitespace, then a token without any.
        constexpr std::string_view scheme = "Bearer";
        const std::string_view header = utils::trimView(auth_res.front());
        if(header.size() <= scheme.size()
            || !utils::equalsIgnoreCase(header.substr(0, scheme.size()), scheme)
            || std::isspace(static_cast<unsigned char>(header[scheme.size()])) == 0)
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE, "Expected a Bearer token"});
        }

        const std::string_view token = utils::trimView(header.substr(scheme.size()));
        if(token.empty() || std::ranges::any_of(token, [](const char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }))
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE, "Expected a single Bearer token"});
        }
        return std::string(token);
    }

    asio::awaitable<std::expected<chain::Address, auth::AuthError>> authenticate(const http::Request & request, const auth::AuthManager & auth_manager)
    {
        const parse::Result<std::string> token_res = _getAccessTokenFromHeader(request);
        if(!token_res)
        {
            spdlog::error("Failed to parse token");
            co_return std::unexpected(auth::AuthError{auth::AuthError::Kind::INVALID_TOKEN});
        }

        const auto verification_res = auth_manager.verifyAccessToken(*token_res);
        if(!verification_res)
        {
            spdlog::error("Failed to verify token");
            co_return std::unexpected(verification_res.error());
        }

        co_return verification_res.value();
    }

    asio::awaitable<http::Response> GET_nonce(const http::Request & request, std::vector<server::RouteArg> args, server::QueryArgsList query_args, auth::AuthManager & auth_manager)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::Connection, "close")
                .setHeader(http::Header::ContentType, "application/json");

        if(args.size() != 1)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid number of arguments. Expected 1 argument."}
                }.dump());

            co_return response;
        }
        const auto address_arg = parse::parseRouteArgAs<std::string>(args.at(0));

        if(!address_arg)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Invalid address argument"}
                }.dump());

            co_return response;
        }

        const std::optional<chain::Address> address_res = chain::addressFromHex(address_arg.value());
        if(!address_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid address value"}
                }.dump());

            co_return response;
        }
        
        // The page names its origin with `origin`, since a browser sends no Origin header on a
        // same-origin GET; otherwise the Origin header of a cross-origin request is used. The
        // message names either only when it is one of ours.
        std::string origin;
        if(query_args.contains("origin"))
        {
            const auto requested = parse::parseRouteArgAs<std::string>(query_args.at("origin"));
            if(!requested || !auth_manager.loginOrigin(*requested))
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "origin is not a configured sign-in origin"}
                    }.dump());

                co_return response;
            }
            origin = *requested;
        }
        else
        {
            const auto origin_header = request.getHeader(http::Header::Origin);
            if(!origin_header.empty()) origin = origin_header.front();
        }

        const auto challenge = auth_manager.issueLogin(*address_res, origin);

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json{
                {"nonce", challenge.nonce},
                {"message", challenge.message}
            }.dump());

        co_return response;
    }

    asio::awaitable<http::Response> OPTIONS_auth(const http::Request & request, std::vector<server::RouteArg>, server::QueryArgsList)
    {
        http::Response response;
        response.setCode(http::Code::NoContent)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::AccessControlAllowMethods, "POST, OPTIONS")
                .setHeader(http::Header::AccessControlAllowHeaders, "Authorization, Content-Type")
                .setHeader(http::Header::AccessControlMaxAge, "600")
                .setHeader(http::Header::Connection, "close");

        co_return response;
    }

    asio::awaitable<http::Response> POST_auth(const http::Request & request, std::vector<server::RouteArg> args, server::QueryArgsList, auth::AuthManager & auth_manager)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::Connection, "close")
                .setHeader(http::Header::ContentType, "application/json");

        if(args.size() != 0)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid number of arguments. Expected no arguments."}
                }.dump());

            co_return response;
        }

        const json auth_request = json::parse(request.getBody(), nullptr, false);
        if(auth_request.is_discarded() || !auth_request.is_object())
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Request body is not a JSON object"}
                }.dump());

            co_return response;
        }

        for(const char * field : {"address", "nonce", "signature"})
        {
            if(!auth_request.contains(field) || !auth_request.at(field).is_string())
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json {
                        {"message", std::format("Missing {}", field)}
                    }.dump());

                co_return response;
            }
        }

        const std::string address_str = auth_request.at("address").get<std::string>();
        auto address_res = chain::addressFromHex(address_str);
        if(!address_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Invalid address"}
                }.dump());

            co_return response;
        }

        const auto access_token = co_await auth_manager.login(
            *address_res, auth_request.at("nonce").get<std::string>(), auth_request.at("signature").get<std::string>());
        if(!access_token)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", access_token.error().kind == auth::AuthError::Kind::INVALID_SIGNATURE
                        ? "Invalid signature"
                        : "Unknown, expired or already redeemed nonce; request a new one from /nonce"}
                }.dump());

            co_return response;
        }

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json {
                {"access_token", *access_token}
            }.dump());

        co_return response;
    }
}