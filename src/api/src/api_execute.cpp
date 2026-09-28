#include "api.hpp"

#include <algorithm>

namespace dcn
{
    namespace
    {
        // The runner allocates particles_count words per scalar, so both bounds cap the
        // work a request can ask for before any EVM runs.
        constexpr std::uint32_t MAX_PARTICLES_COUNT = 65536;
        constexpr int MAX_DYNAMIC_RUNNING_INSTANCES = 4096;

        // Runner.gen's revert reason for a connector its registry does not hold.
        constexpr std::string_view RUNNER_UNKNOWN_CONNECTOR = "cannot find connector";

        http::Response makeResponse()
        {
            http::Response response;
            response.setCode(http::Code::Unknown)
                    .setVersion("HTTP/1.1")
                    .setHeader(http::Header::AccessControlAllowOrigin, "*")
                    .setHeader(http::Header::ContentType, "application/json")
                    .setHeader(http::Header::CacheControl, "no-store")
                    .setHeader(http::Header::Connection, "close");
            return response;
        }

        http::Response failure(const http::Code code, const std::string & message)
        {
            http::Response response = makeResponse();
            response.setCode(code).setBodyWithContentLength(json{{"message", message}}.dump());
            return response;
        }

        // Calldata for Runner.gen(string,uint32,(uint32,uint32,uint32)[]), with the dynamic
        // running instances sorted by position as the runner requires.
        std::vector<std::uint8_t> encodeGenCall(const ExecuteRequest & request)
        {
            std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> dynamic_ri;
            dynamic_ri.reserve(static_cast<std::size_t>(request.dynamic_ri_size()));
            for(const auto & [position, running_instance] : request.dynamic_ri())
            {
                dynamic_ri.emplace_back(position, running_instance.start_point(), running_instance.transformation_shift());
            }
            std::ranges::sort(dynamic_ri, {}, [](const auto & entry) { return std::get<0>(entry); });

            std::vector<std::uint8_t> calldata = chain::constructSelector("gen(string,uint32,(uint32,uint32,uint32)[])");
            const std::vector<std::uint8_t> arguments = pt::abi::encodeTuple({
                pt::abi::stringComponent(request.connector_name()),
                pt::abi::uintComponent(request.particles_count()),
                pt::abi::Component{.dynamic = true, .data = evm::encodeAsArg(dynamic_ri)}});
            calldata.insert(calldata.end(), arguments.begin(), arguments.end());
            return calldata;
        }

        // A revert is the runner refusing this request, not a server fault.
        http::Response rejected(const std::vector<std::uint8_t> & revert_data)
        {
            if(const auto pt_error = parse::decodeBytes<pt::PTExecuteError>(revert_data))
            {
                return failure(http::Code::BadRequest, std::format("Execution rejected: {}", pt_error->kind));
            }

            const auto reason = chain::decodeRevertReason(revert_data);
            if(reason == RUNNER_UNKNOWN_CONNECTOR)
            {
                return failure(http::Code::NotFound, "Connector not found");
            }
            return failure(http::Code::BadRequest, std::format(
                "Execution rejected: {}", reason.value_or(chain::bytesToHex(revert_data))));
        }

        std::expected<json, http::Response> decodeParticles(const std::vector<std::uint8_t> & output)
        {
            const auto particles = parse::decodeBytes<std::vector<Particles>>(output);
            if(!particles)
            {
                return std::unexpected(failure(http::Code::InternalServerError, "Failed to decode particles"));
            }
            auto body = parse::parseToJson(*particles, parse::use_json);
            if(!body)
            {
                return std::unexpected(failure(http::Code::InternalServerError, "Failed to parse json output"));
            }
            return std::move(*body);
        }

        http::Response ok(const json & body)
        {
            http::Response response = makeResponse();
            response.setCode(http::Code::OK).setBodyWithContentLength(body.dump());
            return response;
        }

        /// Authentication and request validation shared by /execute and /simulate.
        asio::awaitable<std::expected<std::pair<ExecuteRequest, chain::Address>, http::Response>> beginExecution(
            const http::Request & request,
            const std::vector<server::RouteArg> & args,
            const auth::AuthManager & auth_manager)
        {
            if(!args.empty())
            {
                co_return std::unexpected(failure(http::Code::BadRequest, "Unexpected arguments"));
            }

            const auto auth_result = co_await authenticate(request, auth_manager);
            if(!auth_result)
            {
                co_return std::unexpected(failure(
                    http::Code::Unauthorized, std::format("Authentication error: {}", auth_result.error().kind)));
            }

            auto execute_request = parse::parseFromJson<ExecuteRequest>(request.getBody(), parse::use_protobuf);
            if(!execute_request)
            {
                co_return std::unexpected(failure(http::Code::BadRequest, "Failed to parse execute request"));
            }
            if(execute_request->particles_count() == 0 || execute_request->particles_count() > MAX_PARTICLES_COUNT)
            {
                co_return std::unexpected(failure(http::Code::BadRequest, std::format(
                    "particles_count must be between 1 and {}", MAX_PARTICLES_COUNT)));
            }
            if(execute_request->dynamic_ri_size() > MAX_DYNAMIC_RUNNING_INSTANCES)
            {
                co_return std::unexpected(failure(http::Code::BadRequest, std::format(
                    "at most {} dynamic running instances are accepted", MAX_DYNAMIC_RUNNING_INSTANCES)));
            }

            co_return std::pair{std::move(*execute_request), *auth_result};
        }
    }

    asio::awaitable<http::Response> OPTIONS_execute(const http::Request &, std::vector<server::RouteArg>, server::QueryArgsList)
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

    asio::awaitable<http::Response> POST_execute(
        const http::Request & request,
        std::vector<server::RouteArg> args,
        server::QueryArgsList,
        const auth::AuthManager & auth_manager,
        const ChainAccess * chain)
    {
        auto begun = co_await beginExecution(request, args, auth_manager);
        if(!begun) co_return std::move(begun.error());

        if(chain == nullptr)
        {
            co_return failure(http::Code::ServiceUnavailable, "On-chain execution is not configured on this server");
        }
        if(chain->runner.empty())
        {
            co_return failure(http::Code::ServiceUnavailable, "No runner is configured for on-chain execution");
        }

        // The tag is resolved to one block first and the call is pinned to that block's
        // hash, so the answer names exactly the chain state it came from.
        const auto block = co_await chain->client->blockByTag(chain->execute_block_tag);
        if(!block || !*block)
        {
            co_return failure(http::Code::ServiceUnavailable, std::format(
                "Cannot read the '{}' block: {}",
                chain->execute_block_tag,
                block ? std::string("the endpoint does not serve it") : block.error().message));
        }
        const rpc::BlockHeader & header = **block;

        // The chain answers: nothing here consults the server's own view of the registry,
        // so a connector counts as soon as it is registered at that block.
        const auto output = co_await chain->client->callAtBlock(
            chain->runner,
            chain::bytesToHex(encodeGenCall(begun->first)),
            header.hash,
            chain->execute_max_gas);
        if(!output)
        {
            if(output.error().kind != rpc::RpcError::Kind::EXECUTION)
            {
                co_return failure(http::Code::ServiceUnavailable, std::format(
                    "On-chain execution is unavailable: {}", output.error().message));
            }
            const auto revert_data = evmc::from_hex(output.error().data);
            if(revert_data && !revert_data->empty())
            {
                co_return rejected(std::vector<std::uint8_t>(revert_data->begin(), revert_data->end()));
            }
            // Out of gas under the cap, or a provider-side execution limit.
            co_return failure(http::Code::BadRequest, std::format(
                "Execution rejected: {}", output.error().message));
        }

        const auto bytes = evmc::from_hex(*output);
        if(!bytes)
        {
            co_return failure(http::Code::BadGateway, "Chain returned a malformed result");
        }
        auto particles = decodeParticles(std::vector<std::uint8_t>(bytes->begin(), bytes->end()));
        if(!particles) co_return std::move(particles.error());

        // Anyone can check the result by calling the runner at the same block.
        co_return ok(json{
            {"block_number", header.number},
            {"block_hash", header.hash},
            {"runner", chain->runner},
            {"particles", std::move(*particles)}});
    }

    asio::awaitable<http::Response> POST_simulate(
        const http::Request & request,
        std::vector<server::RouteArg> args,
        server::QueryArgsList,
        const auth::AuthManager & auth_manager,
        registry::Registry & registry,
        evm::EVM & evm,
        const config::Config & config)
    {
        auto begun = co_await beginExecution(request, args, auth_manager);
        if(!begun) co_return std::move(begun.error());
        const auto & [execute_request, caller] = *begun;

        if(!co_await registry.hasLocalEntity(pt::EntityKind::CONNECTOR, execute_request.connector_name()))
        {
            co_return failure(http::Code::NotFound, "Connector not found");
        }

        if(!co_await loader::ensureConnectorDeployed(
            evm, registry, execute_request.connector_name(), config.storage_path))
        {
            co_return failure(http::Code::InternalServerError, "Connector deployment invariant failed");
        }

        co_await evm.setGas(caller, evm::DEFAULT_GAS_LIMIT);
        co_await evm.setGas(evm.getRunnerAddress(), evm::DEFAULT_GAS_LIMIT);
        const auto exec_result = co_await evm.execute(
            caller, evm.getRunnerAddress(), encodeGenCall(execute_request), evm::DEFAULT_GAS_LIMIT, 0);
        if(!exec_result)
        {
            if(exec_result.error().kind == chain::ExecuteError::Kind::TRANSACTION_REVERTED)
            {
                co_return rejected(exec_result.error().result_bytes);
            }
            co_return failure(http::Code::InternalServerError, std::format(
                "Failed to execute code : {}", exec_result.error().kind));
        }

        auto particles = decodeParticles(exec_result.value());
        if(!particles) co_return std::move(particles.error());
        co_return ok(*particles);
    }
}
