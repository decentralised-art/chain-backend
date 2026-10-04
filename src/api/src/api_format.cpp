#include "api.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace dcn
{
    namespace
    {
        constexpr std::size_t MAX_LIMIT = 256;
    }

    asio::awaitable<http::Response> OPTIONS_formats(const http::Request &, std::vector<server::RouteArg>, server::QueryArgsList)
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

    asio::awaitable<http::Response> HEAD_formats(
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
                    return parse::parseFormatCursorHex(token);
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

    asio::awaitable<http::Response> GET_formats(
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

        std::optional<evmc::bytes32> after_format;
        if(query_args.contains("after"))
        {
            const auto after_res = parse::parseRouteArgAs<std::string>(query_args.at("after"))
                .and_then([&](const std::string & token)
                {
                    return parse::parseFormatCursorHex(token);
                });

            if(!after_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid after cursor"}
                    }.dump());
                co_return response;
            }

            after_format = *after_res;
        }

        const std::size_t limit = *limit_res;
        const std::size_t total_formats = co_await registry.getFormatsCount();
        const auto page = co_await registry.getFormatsCursor(after_format, limit);

        json json_output;
        json_output["limit"] = limit;
        json_output["total_formats"] = total_formats;
        json_output["cursor"] = json::object();
        json_output["cursor"]["has_more"] = page.has_more;
        json_output["cursor"]["next_after"] = page.next_after.has_value() ? json(*page.next_after) : json(nullptr);
        json_output["formats"] = json::array();
        for(const std::string & format_hash_hex : page.entries)
        {
            json_output["formats"].emplace_back(format_hash_hex);
        }

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json_output.dump());
        co_return response;
    }

    asio::awaitable<http::Response> OPTIONS_format(const http::Request &, std::vector<server::RouteArg>, server::QueryArgsList)
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

    asio::awaitable<http::Response> GET_format(
        const http::Request & request,
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

        if(args.size() != 1)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid number of arguments"}
                }.dump());

            co_return response;
        }

        const auto format_hash_arg = parse::parseRouteArgAs<std::string>(args.at(0));
        if(!format_hash_arg)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid format hash argument"}
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

        const std::optional<evmc::bytes32> format_hash_res = evmc::from_hex<evmc::bytes32>(format_hash_arg.value());
        if(!format_hash_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid format hash"}
                }.dump());

            co_return response;
        }

        auto limit_res = parse::parseRouteArgAs<std::size_t>(query_args.at("limit"));
        if(limit_res && (limit_res.value() == 0 || limit_res.value() > MAX_LIMIT))
        {
            limit_res = std::unexpected(parse::ParseError{parse::ParseError::Kind::OUT_OF_RANGE});
        }

        std::optional<registry::NameCursor> after_name;
        if(query_args.contains("after"))
        {
            const auto after_res =
                parse::parseRouteArgAs<std::string>(query_args.at("after"))
                .and_then([&](const std::string & s) {
                    return parse::parseNameCursor(s);
                });

            if(!after_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid after cursor"}
                    }.dump());

                co_return response;
            }

            after_name = after_res.value();
        }

        if(!limit_res)
        {
            std::string msg_str = "Invalid argument limit.";
            if(!limit_res) msg_str += std::format(" limit error: {}.", limit_res.error().kind);

            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", msg_str}
                }.dump());

            co_return response;
        }

        const std::size_t limit = limit_res.value();
        const std::size_t total_connectors = co_await registry.getFormatConnectorNamesCount(*format_hash_res);
        const auto page = co_await registry.getFormatConnectorNamesCursor(*format_hash_res, after_name, limit);
        const std::vector<std::string> & connector_names = page.entries;

        json json_output;
        json_output["format_hash"] = evmc::hex(*format_hash_res);
        json_output["limit"] = limit;
        json_output["total_connectors"] = total_connectors;

        json_output["cursor"] = json::object();
        json_output["cursor"]["has_more"] = page.has_more;
        if(page.next_after.has_value())
        {
            json_output["cursor"]["next_after"] = *page.next_after;
        }
        else
        {
            json_output["cursor"]["next_after"] = nullptr;
        }
        json_output["scalars"] = json::array();
        const auto scalar_labels_res = co_await registry.getScalarLabelsByFormatHash(*format_hash_res);
        if(scalar_labels_res.has_value())
        {
            std::vector<std::string> scalar_tail_entries;
            scalar_tail_entries.reserve(scalar_labels_res->size());
            for(const auto & scalar_label : *scalar_labels_res)
            {
                scalar_tail_entries.push_back(std::format("{}:{}", scalar_label.scalar, scalar_label.tail_id));
            }

            std::sort(scalar_tail_entries.begin(), scalar_tail_entries.end());
            for(const auto & entry : scalar_tail_entries)
            {
                json_output["scalars"].emplace_back(entry);
            }
        }
        json_output["connectors"] = json::array();

        for(const std::string & connector_name : connector_names)
        {
            json_output["connectors"].emplace_back(connector_name);
        }

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json_output.dump());

        co_return response;
    }
}
