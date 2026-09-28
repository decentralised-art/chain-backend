#include "api.hpp"
#include "address.hpp"

// TODO
// ABI offset encoding (correctness)
// Add size limits (DoS protection)
// Define a real execution budget / gas policy

namespace dcn
{
    asio::awaitable<http::Response> HEAD_connector(const http::Request & request, std::vector<server::RouteArg> args, server::QueryArgsList, registry::Registry & registry)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::CacheControl, "no-store")
                .setHeader(http::Header::ContentLength, "0")
                .setHeader(http::Header::Connection, "close");

        // Validate path: /connector/<name>
        if(args.size() != 1) {
            response.setCode(http::Code::BadRequest);
            co_return response;
        }

        const auto connector_name_result = parse::parseRouteArgAs<std::string>(args.at(0));
        if (!connector_name_result) {
            response.setCode(http::Code::BadRequest);
            co_return response;
        }
        const auto & connector_name = connector_name_result.value();

        std::optional<registry::ConnectorRecordHandle> connector_record_res =
            co_await registry.getConnectorRecordHandle(connector_name);

        if (!connector_record_res) {
            // connector not found
            response.setCode(http::Code::NotFound);
            co_return response;
        }

        // connector exists
        response.setCode(http::Code::OK);
        co_return response;
    }

    asio::awaitable<http::Response> OPTIONS_connector(const http::Request & request, std::vector<server::RouteArg>, server::QueryArgsList)
    {
        http::Response response;
        response.setCode(http::Code::NoContent)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::AccessControlAllowMethods, "HEAD, GET, POST, OPTIONS")
                .setHeader(http::Header::AccessControlAllowHeaders, "Authorization, Content-Type")
                .setHeader(http::Header::AccessControlMaxAge, "600")
                .setHeader(http::Header::Connection, "close");

        co_return response;
    }

    asio::awaitable<http::Response> GET_connector(
        const http::Request & request,
        std::vector<server::RouteArg> args,
        server::QueryArgsList,
        registry::Registry & registry)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::ContentType, "application/json")
                .setHeader(http::Header::CacheControl, "no-store")
                .setHeader(http::Header::Connection, "close");

        if(args.size() != 1)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid number of arguments. Expected 1 argument."}
                }.dump());

            co_return response;
        }

        auto connector_name_result = parse::parseRouteArgAs<std::string>(args.at(0));

        if(!connector_name_result)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid connector name"}
                }.dump());

            co_return response;
        }
        std::optional<registry::ConnectorRecordHandle> connector_record_res =
            co_await registry.getConnectorRecordHandle(connector_name_result.value());

        if(!connector_record_res)
        {
            response.setCode(http::Code::NotFound)
                .setBodyWithContentLength(json {
                    {"message", "Connector not found"}
                }.dump());

            co_return response;
        }
        
        auto json_res = parse::parseToJson((*connector_record_res)->connector(), parse::use_json);

        if(!json_res)
        {
            response.setCode(http::Code::InternalServerError)
                .setBodyWithContentLength(json {
                    {"message", "Parsing response to json failed"}
                }.dump());

            co_return response;
        }

        std::optional<evmc::bytes32> format_hash =
            co_await registry.getFormatHash(connector_name_result.value());

        if(!format_hash)
        {
            response.setCode(http::Code::InternalServerError)
                .setBodyWithContentLength(json{
                    {"message", "Failed to fetch connector format hash"}
                }.dump());

            co_return response;
        }

        (*json_res)["owner"] = (*connector_record_res)->owner();
        const auto location = co_await registry.getEntityLocation(
            registry::EntityKind::CONNECTOR, connector_name_result.value());
        // A local simulation entity has no chain address; the response keeps the 0x0 sentinel.
        (*json_res)["address"] = (location && location->address != chain::Address{})
            ? chain::addressToHex(location->address)
            : "0x0";
        (*json_res)["format_hash"] = evmc::hex(*format_hash);

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json_res->dump());

        co_return response;
    }

    asio::awaitable<http::Response> POST_connector(const http::Request & request, std::vector<server::RouteArg> args, server::QueryArgsList,
        auth::AuthManager & auth_manager, registry::Registry & registry, evm::EVM & evm, const config::Config & config, const ChainAccess * chain)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::ContentType, "application/json")
                .setHeader(http::Header::Connection, "close");

        if(!args.empty())
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Unexpected arguments"}
                }.dump());

            co_return response;
        }

        const auto auth_result = co_await authenticate(request, auth_manager);

        if(!auth_result)
        {
            response.setCode(http::Code::Unauthorized)
                .setBodyWithContentLength(json {
                    {"message", std::format("Authentication error: {}", auth_result.error().kind)}
                }.dump());

            co_return response;
        }
        const auto & address = auth_result.value();

        spdlog::debug(std::format("token verified address : {}", address));

        // parse connector from json_string
        const auto connector_res = parse::parseFromJson<Connector>(request.getBody(), parse::use_protobuf);

        if(!connector_res) 
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Failed to parse connector"}
                }.dump());

            co_return response;
        }

        const Connector & connector = *connector_res;

        if(auto refused = co_await admitLocalEntityName(registry, chain, pt::EntityKind::CONNECTOR, connector.name()))
        {
            co_return std::move(*refused);
        }

        ConnectorRecord connector_record;
        connector_record.set_owner(chain::addressToHex(address));
        *connector_record.mutable_connector() = std::move(connector);

        const auto deploy_res = co_await loader::deployConnector(evm, registry, connector_record, config.storage_path);
        if(!deploy_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", std::format("Failed to deploy connector. Error: {}", deploy_res.error().kind)}
                }.dump());

            co_return response;
        }

        // Compute the canonical, composite-aware format hash deterministically from the
        // connector definition. This does not depend on the EVM having processed/emitted
        // the deploy log, so the response is immediate and reproducible.
        const auto format_hash_opt = co_await registry.computeConnectorFormatHash(connector_record.connector());
        if(!format_hash_opt)
        {
            response.setCode(http::Code::InternalServerError)
                .setBodyWithContentLength(json{
                    {"message", "Failed to compute connector format hash"}
                }.dump());

            co_return response;
        }

        json json_output;
        json_output["name"] = connector_record.connector().name();
        json_output["owner"] = connector_record.owner();
        json_output["address"] = "0x0";
        json_output["format_hash"] = evmc::hex(*format_hash_opt);

        response.setCode(http::Code::Created)
            .setBodyWithContentLength(json_output.dump());

        co_return response;
    }
}

