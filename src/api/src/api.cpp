#include "api.hpp"

#include "pt_reads.hpp"

namespace dcn
{
    asio::awaitable<std::optional<http::Response>> admitLocalEntityName(
        registry::Registry & registry,
        const ChainAccess * chain,
        const pt::EntityKind kind,
        const std::string & name)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::ContentType, "application/json")
                .setHeader(http::Header::Connection, "close");
        const auto refuse = [&](const http::Code code, const std::string & message)
        {
            response.setCode(code).setBodyWithContentLength(json{{"message", message}}.dump());
            return std::optional<http::Response>(std::move(response));
        };

        if(!pt::isValidEntityName(name))
        {
            co_return refuse(http::Code::BadRequest, std::format(
                "Name must start with a letter or '_', contain only letters, digits and '_', "
                "and be at most {} characters", pt::MAX_ENTITY_NAME_LENGTH));
        }

        const std::string reserved = std::format("{} name is reserved on chain", pt::entityKindName(kind));
        if(co_await registry.hasRemoteEntity(kind, name))
        {
            co_return refuse(http::Code::Conflict, reserved);
        }
        if(chain == nullptr)
        {
            co_return std::nullopt;
        }

        const auto contains = co_await rpc::registryContainsEntity(*chain->client, chain::addressToHex(chain->registry), kind, name);
        if(!contains)
        {
            spdlog::warn("Live on-chain name check for '{}' failed: {}", name, contains.error().message);
            co_return refuse(http::Code::ServiceUnavailable, "Live on-chain name check is unavailable");
        }
        if(*contains)
        {
            co_return refuse(http::Code::Conflict, reserved);
        }
        co_return std::nullopt;
    }

    asio::awaitable<http::Response> GET_version(const http::Request & request, std::vector<server::RouteArg>, server::QueryArgsList, const std::string & build_timestamp)
    {
        http::Response response;
        response.setCode(http::Code::OK)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::CacheControl, "public, max-age=60")
                .setHeader(http::Header::ContentType, "application/json")
                .setHeader(http::Header::Connection, "close")
                .setBodyWithContentLength(json {
                    {"version", std::format("{}.{}.{}", dcn::MAJOR_VERSION, dcn::MINOR_VERSION, dcn::PATCH_VERSION)}, 
                    {"build_timestamp", build_timestamp}
                }.dump());

        co_return response;
    }
}