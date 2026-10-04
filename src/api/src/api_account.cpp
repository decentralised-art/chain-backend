#include "api.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string>

namespace dcn
{
    namespace
    {
        constexpr std::size_t MAX_LIMIT = 256;

        parse::Result<chain::Address> parseAccountCursorHex(const std::string & token)
        {
            const auto trimmed_token = utils::trimAsciiWhitespace(token);
            if(!trimmed_token.has_value())
            {
                return std::unexpected(parse::ParseError{
                    .kind = parse::ParseError::Kind::INVALID_VALUE,
                    .message = "Account cursor cannot be empty"});
            }

            std::string normalized = *trimmed_token;
            if(normalized.size() >= 2 && normalized.at(0) == '0' &&
               (normalized.at(1) == 'x' || normalized.at(1) == 'X'))
            {
                normalized.erase(0, 2);
            }

            if(normalized.size() != 40)
            {
                return std::unexpected(parse::ParseError{
                    .kind = parse::ParseError::Kind::INVALID_VALUE,
                    .message = "Account cursor must be 20-byte hex"});
            }

            for(char & ch : normalized)
            {
                if(std::isxdigit(static_cast<unsigned char>(ch)) == 0)
                {
                    return std::unexpected(parse::ParseError{
                        .kind = parse::ParseError::Kind::INVALID_VALUE,
                        .message = "Account cursor must be hex"});
                }
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }

            const auto parsed = evmc::from_hex<chain::Address>(normalized);
            if(!parsed.has_value())
            {
                return std::unexpected(parse::ParseError{
                    .kind = parse::ParseError::Kind::INVALID_VALUE,
                    .message = "Account cursor parse failed"});
            }

            return *parsed;
        }
    }

    asio::awaitable<http::Response> OPTIONS_accounts(const http::Request &, std::vector<server::RouteArg>, server::QueryArgsList)
    {
        http::Response response;
        response.setCode(http::Code::NoContent)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::AccessControlAllowMethods, "HEAD, GET, OPTIONS")
                .setHeader(http::Header::AccessControlAllowHeaders, "Content-Type")
                .setHeader(http::Header::AccessControlMaxAge, "600")
                .setHeader(http::Header::Connection, "close");

        co_return response;
    }

    asio::awaitable<http::Response> HEAD_accounts(
        const http::Request &,
        std::vector<server::RouteArg> args,
        server::QueryArgsList query_args,
        registry::Registry &)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::ContentLength, "0")
                .setHeader(http::Header::Connection, "close");

        if(!args.empty())
        {
            response.setCode(http::Code::BadRequest);
            co_return response;
        }

        if(query_args.contains("limit") == false)
        {
            response.setCode(http::Code::BadRequest);
            co_return response;
        }

        auto limit_res = parse::parseRouteArgAs<std::size_t>(query_args.at("limit"));
        if(limit_res && (limit_res.value() == 0 || limit_res.value() > MAX_LIMIT))
        {
            limit_res = std::unexpected(parse::ParseError{parse::ParseError::Kind::OUT_OF_RANGE});
        }

        if(!limit_res)
        {
            response.setCode(http::Code::BadRequest);
            co_return response;
        }

        if(query_args.contains("after"))
        {
            const auto after_res = parse::parseRouteArgAs<std::string>(query_args.at("after"))
                .and_then([&](const std::string & token)
                {
                    return parseAccountCursorHex(token);
                });

            if(!after_res)
            {
                response.setCode(http::Code::BadRequest);
                co_return response;
            }
        }

        response.setCode(http::Code::OK);
        co_return response;
    }

    asio::awaitable<http::Response> GET_accounts(
        const http::Request &,
        std::vector<server::RouteArg> args,
        server::QueryArgsList query_args,
        registry::Registry & registry)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::Connection, "close")
                .setHeader(http::Header::ContentType, "application/json");

        if(!args.empty())
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid number of arguments"}
                }.dump());
            co_return response;
        }

        if(query_args.contains("limit") == false)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Missing argument limit"}
                }.dump());
            co_return response;
        }

        auto limit_res = parse::parseRouteArgAs<std::size_t>(query_args.at("limit"));
        if(limit_res && (limit_res.value() == 0 || limit_res.value() > MAX_LIMIT))
        {
            limit_res = std::unexpected(parse::ParseError{parse::ParseError::Kind::OUT_OF_RANGE});
        }

        if(!limit_res)
        {
            std::string msg_str = "Invalid argument limit.";
            msg_str += std::format(" limit error: {}.", limit_res.error().kind);

            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", msg_str}
                }.dump());
            co_return response;
        }

        std::optional<chain::Address> after_account;
        if(query_args.contains("after"))
        {
            const auto after_res = parse::parseRouteArgAs<std::string>(query_args.at("after"))
                .and_then([&](const std::string & token)
                {
                    return parseAccountCursorHex(token);
                });

            if(!after_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid after cursor"}
                    }.dump());
                co_return response;
            }

            after_account = *after_res;
        }

        const std::size_t limit = *limit_res;
        const std::size_t total_accounts = co_await registry.getAccountsCount();
        const auto page = co_await registry.getAccountsCursor(after_account, limit);

        json json_output;
        json_output["limit"] = limit;
        json_output["total_accounts"] = total_accounts;
        json_output["cursor"] = json::object();
        json_output["cursor"]["has_more"] = page.has_more;
        json_output["cursor"]["next_after"] = page.next_after.has_value() ? json(*page.next_after) : json(nullptr);
        json_output["accounts"] = json::array();
        for(const std::string & account_hex : page.entries)
        {
            json_output["accounts"].emplace_back(account_hex);
        }

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json_output.dump());
        co_return response;
    }

    asio::awaitable<http::Response> OPTIONS_accountInfo(const http::Request & request, std::vector<server::RouteArg>, server::QueryArgsList)
    {
        http::Response response;
        response.setCode(http::Code::NoContent)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::AccessControlAllowMethods, "GET, OPTIONS")
                .setHeader(http::Header::AccessControlAllowHeaders, "Content-Type")
                .setHeader(http::Header::AccessControlMaxAge, "600")
                .setHeader(http::Header::Connection, "close");

        co_return response;
    }

    asio::awaitable<http::Response> GET_accountInfo(const http::Request & request, std::vector<server::RouteArg> args, server::QueryArgsList query_args, registry::Registry & registry)
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
                    {"message", "Invalid number of arguments"}
                }.dump());

            co_return response;
        }
        auto address_arg = parse::parseRouteArgAs<std::string>(args.at(0));

        if(!address_arg)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid address argument"}
                }.dump());

            co_return response;
        }

        if(query_args.contains("limit") == false)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Missing argument limit"}
                }.dump());

            co_return response;
        }

        std::optional<chain::Address> address_res = chain::addressFromHex(address_arg.value());
        if(!address_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid address"}
                }.dump());

            co_return response;
        }
        const auto & address = address_res.value();

        auto limit_res = parse::parseRouteArgAs<std::size_t>(query_args.at("limit"));
        if(limit_res && (limit_res.value() == 0 || limit_res.value() > MAX_LIMIT))
        {
            limit_res = std::unexpected(parse::ParseError{parse::ParseError::Kind::OUT_OF_RANGE});
        }
        if(!limit_res)
        {
            std::string msg_str = "Invalid argument limit.";
            if(!limit_res) msg_str += std::format(" limit error: {}.", limit_res.error().kind);

            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", msg_str}
                }.dump());

            co_return response;
        }

        std::optional<registry::NameCursor> after_connectors;
        std::optional<registry::NameCursor> after_transformations;
        std::optional<registry::NameCursor> after_conditions;

        if(query_args.contains("after_connectors"))
        {
            const auto after_connectors_res =
                parse::parseRouteArgAs<std::string>(query_args.at("after_connectors"))
                .and_then([&](const std::string & token) { return parse::parseNameCursor(token); });

            if(!after_connectors_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid after_connectors cursor"}
                    }.dump());
                co_return response;
            }

            after_connectors = std::move(after_connectors_res.value());
        }

        if(query_args.contains("after_transformations"))
        {
            const auto after_transformations_res =
                parse::parseRouteArgAs<std::string>(query_args.at("after_transformations"))
                .and_then([&](const std::string & token) { return parse::parseNameCursor(token); });

            if(!after_transformations_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid after_transformations cursor"}
                    }.dump());
                co_return response;
            }

            after_transformations = std::move(after_transformations_res.value());
        }

        if(query_args.contains("after_conditions"))
        {
            const auto after_conditions_res =
                parse::parseRouteArgAs<std::string>(query_args.at("after_conditions"))
                .and_then([&](const std::string & token) { return parse::parseNameCursor(token); });

            if(!after_conditions_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid after_conditions cursor"}
                    }.dump());
                co_return response;
            }

            after_conditions = std::move(after_conditions_res.value());
        }

        const std::size_t limit = limit_res.value();
        const auto connectors_page = co_await registry.getOwnedConnectorsCursor(address, after_connectors, limit);
        const auto transformations_page = co_await registry.getOwnedTransformationsCursor(address, after_transformations, limit);
        const auto conditions_page = co_await registry.getOwnedConditionsCursor(address, after_conditions, limit);

        json json_output;

        json_output["owned_connectors"] = json::array();
        for(const auto & name : connectors_page.entries)
        {
            json_output["owned_connectors"].push_back(name);
        }

        json_output["owned_transformations"] = json::array();
        for(const auto & name : transformations_page.entries)
        {
            json_output["owned_transformations"].push_back(name);
        }

        json_output["owned_conditions"] = json::array();
        for(const auto & name : conditions_page.entries)
        {
            json_output["owned_conditions"].push_back(name);
        }

        json_output["address"] = evmc::hex(address);
        json_output["limit"] = limit;

        json_output["cursor_connectors"] = json::object();
        json_output["cursor_connectors"]["has_more"] = connectors_page.has_more;
        json_output["cursor_connectors"]["next_after"] =
            connectors_page.next_after.has_value() ? json(registry::serializeNameCursor(*connectors_page.next_after)) : json(nullptr);

        json_output["cursor_transformations"] = json::object();
        json_output["cursor_transformations"]["has_more"] = transformations_page.has_more;
        json_output["cursor_transformations"]["next_after"] =
            transformations_page.next_after.has_value() ? json(registry::serializeNameCursor(*transformations_page.next_after)) : json(nullptr);

        json_output["cursor_conditions"] = json::object();
        json_output["cursor_conditions"]["has_more"] = conditions_page.has_more;
        json_output["cursor_conditions"]["next_after"] =
            conditions_page.next_after.has_value() ? json(registry::serializeNameCursor(*conditions_page.next_after)) : json(nullptr);

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json_output.dump());

        co_return response;
    }
}
