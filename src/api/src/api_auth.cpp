#include "api.hpp"

namespace dcn
{
    static const std::string ACCESS_TOKEN_PREFIX = "access_token=";

    static parse::Result<std::string> _getAccessTokenFromHeader(const http::Request & request) 
    {
        const auto auth_res = request.getHeader(http::Header::Authorization);
        if(auth_res.empty())
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE, "Authorization header not found"});
        }

        const std::string auth_header = std::accumulate(auth_res.begin(), auth_res.end(), std::string(""));

        if (auth_header.empty()) {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE, "Header string is empty"});
        }

        // `Bearer`, whitespace, then the token up to the next whitespace. A plain scan: the
        // header is unauthenticated input, and std::regex recurses per character.
        constexpr std::string_view scheme = "Bearer";
        constexpr std::string_view whitespace = " \t\n\v\f\r";
        const std::string_view header = auth_header;
        for(std::size_t pos = header.find(scheme); pos != std::string_view::npos; pos = header.find(scheme, pos))
        {
            pos += scheme.size();
            const std::size_t token_begin = header.find_first_not_of(whitespace, pos);
            if(token_begin == pos || token_begin == std::string_view::npos) continue;

            const std::size_t token_end = header.find_first_of(whitespace, token_begin);
            return std::string(header.substr(token_begin, token_end - token_begin));
        }

        return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE, "Token not found in header"});
    }

    asio::awaitable<std::expected<chain::Address, auth::AuthError>> authenticate(const http::Request & request, const auth::AuthManager & auth_manager)
    {
        // try to obtain token from authorization header
        const parse::Result<std::string> token_res = _getAccessTokenFromHeader(request);

        if (!token_res) 
        {
            spdlog::error("Failed to parse token");
            co_return std::unexpected(auth::AuthError{auth::AuthError::Kind::INVALID_TOKEN});
        }
        const std::string & token = token_res.value();

        const auto verification_res = co_await auth_manager.verifyAccessToken(token);

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

        const std::optional<chain::Address> address_res = evmc::from_hex<chain::Address>(address_arg.value());
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

        const auto challenge = co_await auth_manager.issueLogin(*address_res, origin);
        if(!challenge)
        {
            spdlog::warn("Refusing a sign-in message: {} are already pending", auth::MAX_PENDING_LOGINS);
            response.setCode(http::Code::ServiceUnavailable)
                .setBodyWithContentLength(json{
                    {"message", "Too many sign-ins are pending; try again shortly"}
                }.dump());

            co_return response;
        }

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json{
                {"nonce", challenge->nonce},
                {"message", challenge->message}
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
                    {"message", "Invalid number of arguments. Expected 1 argument."}
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
        const std::string signature = auth_request.at("signature").get<std::string>();
        const std::string nonce = auth_request.at("nonce").get<std::string>();

        auto address_res = evmc::from_hex<chain::Address>(address_str);
        if(!address_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Invalid address"}
                }.dump());

            co_return response;
        }
        const chain::Address & address = address_res.value();

        // The signature is checked against the message this server issued under the nonce.
        const auto message = co_await auth_manager.consumeLogin(address, nonce);
        if(!message)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Unknown or expired nonce; request a new one from /nonce"}
                }.dump());

            co_return response;
        }

        if(co_await auth_manager.verifySignature(address, signature, *message) == false)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Invalid signature"}
                }.dump());
                
            co_return response;
        }

        const std::string access_token = co_await auth_manager.generateAccessToken(address);
        
        // set access token in authorization header
        // Authorization: Bearer <token>
        // also include access token in response
        response.setCode(http::Code::OK)
            .setHeader(http::Header::Authorization, std::format("Bearer {}", access_token))
            .setBodyWithContentLength(json {
                {"access_token", access_token}
            }.dump());

        co_return response;
    }
}