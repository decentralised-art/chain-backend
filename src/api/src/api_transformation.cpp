#include "api.hpp"
#include "address.hpp"

// TODO
// ABI offset encoding (correctness)
// Add size limits (DoS protection)
// Define a real execution budget / gas policy

namespace dcn
{

    asio::awaitable<http::Response> HEAD_transformation(const http::Request & request, std::vector<server::RouteArg> args, server::QueryArgsList, registry::Registry & registry)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::CacheControl, "no-store")
                .setHeader(http::Header::ContentLength, "0")
                .setHeader(http::Header::Connection, "close");

        // Expect /transformation/<name>
        if(args.size() != 1) {
            response.setCode(http::Code::BadRequest);
            co_return response;
        }

        const auto transformation_name_result = parse::parseRouteArgAs<std::string>(args.at(0));

        if (!transformation_name_result) {
            response.setCode(http::Code::BadRequest);
            co_return response;
        }

        const auto & transformation_name = transformation_name_result.value();

        std::optional<registry::TransformationRecordHandle> transformation_record_res =
            co_await registry.getTransformationRecordHandle(transformation_name);

        if (!transformation_record_res) {
            // Transformation not found
            response.setCode(http::Code::NotFound);
            co_return response;
        }

        // Transformation exists
        response.setCode(http::Code::OK);
        co_return response;
    }

    asio::awaitable<http::Response> OPTIONS_transformation(const http::Request & request, std::vector<server::RouteArg>, server::QueryArgsList)
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

    asio::awaitable<http::Response> GET_transformation(
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
                .setBodyWithContentLength(json {
                    {"message", "Invalid number of arguments. Expected 1 argument."}
                }.dump());

            co_return response;
        }

        const auto transformation_name_result = parse::parseRouteArgAs<std::string>(args.at(0));

        if(!transformation_name_result)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "Invalid transformation name"}
                }.dump());

            co_return response;
        }
        std::optional<registry::TransformationRecordHandle> transformation_record_res =
            co_await registry.getTransformationRecordHandle(transformation_name_result.value());

        if(!transformation_record_res)
        {
            response.setCode(http::Code::NotFound)
                .setBodyWithContentLength(json {
                    {"message", "Transformation not found"}
                }.dump());

            co_return response;
        }
        
        const auto location = co_await registry.getEntityLocation(
            registry::EntityKind::TRANSFORMATION, transformation_name_result.value());

        // The deployed runtime bytecode; null in the response until the registry holds it.
        Transformation transformation = (*transformation_record_res)->transformation();
        if(location && !location->source.runtime_code_hash.empty())
        {
            const auto runtime_code = co_await registry.getRuntimeCode(location->source.runtime_code_hash);
            if(runtime_code) transformation.set_runtime_code(chain::bytesToHex(*runtime_code));
        }

        auto json_res = parse::parseToJson(std::move(transformation), parse::use_json);

        if(!json_res)
        {
            response.setCode(http::Code::InternalServerError)
                .setBodyWithContentLength(json {
                    {"message", "Cannot parse transformation to JSON"}
                }.dump());

            co_return response;
        }

        (*json_res)["owner"] = (*transformation_record_res)->owner();
        // A local simulation entity has no chain address; the response keeps the 0x0 sentinel.
        (*json_res)["address"] = (location && location->address != chain::Address{})
            ? chain::addressToHex(location->address)
            : "0x0";

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(json_res->dump());
        
        co_return response;
    }

    asio::awaitable<http::Response> POST_transformation(const http::Request & request, std::vector<server::RouteArg> args, server::QueryArgsList,
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

        spdlog::debug(std::format("token verified address: {}", address));
        
        const auto transformation_res = parse::parseFromJson<Transformation>(request.getBody(), parse::use_protobuf);

        if(!transformation_res) 
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", std::format("Failed to parse transformation: {}", transformation_res.error().kind)}
                }.dump());

            co_return response;
        }

        const Transformation & transformation = *transformation_res;

        if(transformation.has_runtime_code())
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", "runtime_code is read from the deployed contract and cannot be set"}
                }.dump());

            co_return response;
        }

        if(auto refused = co_await admitLocalEntityName(registry, chain, pt::EntityKind::TRANSFORMATION, transformation.name()))
        {
            co_return std::move(*refused);
        }

        TransformationRecord transformation_record;
        transformation_record.set_owner(chain::addressToHex(address));
        *transformation_record.mutable_transformation() = std::move(transformation);

        // args_count is chain-derivable from the source (matches the value registered on chain).
        // Record it so the saved JSON artifact and the response carry it explicitly.
        const auto args_count_res = countTransformationArgs(transformation_record.transformation().sol_src());
        if(!args_count_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", std::format("Failed to parse transformation: {}", args_count_res.error().message)}
                }.dump());

            co_return response;
        }
        transformation_record.mutable_transformation()->set_args_count(*args_count_res);

        const auto deploy_res = co_await loader::deployTransformation(evm, registry, transformation_record, config.storage_path);
        if(!deploy_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json {
                    {"message", std::format("Failed to deploy transformation. Error: {}", deploy_res.error().kind)}
                }.dump());

            co_return response;
        }

        json json_output;
        json_output["name"] = transformation_record.transformation().name();
        json_output["owner"] = transformation_record.owner();
        json_output["address"] = "0x0";
        json_output["args_count"] = transformation_record.transformation().args_count();

        response.setCode(http::Code::Created)
            .setBodyWithContentLength(json_output.dump());

        co_return response;
    }
}

