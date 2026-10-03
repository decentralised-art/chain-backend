#pragma once
#include <ranges>

#include <spdlog/spdlog.h>

#include "async.hpp"
#include "utils.hpp"
#include "config.hpp"
#include "route.hpp"
#include "parser.hpp"
#include "pt.hpp"
#include "auth.hpp"
#include "chain.hpp"
#include "evm.hpp"
#include "version.hpp"
#include "loader.hpp"
#include "events.hpp"
#include "feed_types.hpp"
#include "eth_client.hpp"

namespace dcn::publish
{
    class Publisher;
}

namespace dcn
{
    /**
     * @brief Helper function to handle authentication.
     */
    asio::awaitable<std::expected<chain::Address, auth::AuthError>> authenticate(const http::Request & request, const auth::AuthManager & auth_manager);

    /**
     * @brief Live reads the HTTP API makes against the configured chain deployment.
     *
     * Handlers receive a null pointer when the server has no RPC endpoint; then only
     * local simulation state exists.
     */
    struct ChainAccess
    {
        std::shared_ptr<rpc::EthClient> client;
        chain::Address registry{};   //!< PT registry proxy
        std::string runner;     //!< Runner proxy; Runner.gen looks connectors up in `registry`
        std::uint64_t execute_max_gas = 100'000'000;
        std::string execute_block_tag = std::string(rpc::BLOCK_TAG_SAFE);
    };

    /**
     * @brief Admits the name of a new local entity, or answers why not.
     *
     * The name must be a valid identifier and must not be owned by a chain registry:
     * the projected registry is asked first, then the live registry at `latest`, so
     * projector lag cannot admit a name the chain already owns. A live check that
     * cannot complete refuses the name.
     *
     * @return nullopt when the name is admitted, otherwise the response to send
     */
    asio::awaitable<std::optional<http::Response>> admitLocalEntityName(
        registry::Registry & registry,
        const ChainAccess * chain,
        pt::EntityKind kind,
        const std::string & name);


    /**
     * @brief Handles GET requests for the version endpoint.
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param build_timestamp Build timestamp
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_version(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, const std::string & build_timestamp);


    /**
     * @brief Handles HEAD requests for a file.
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> HEAD_serveFile(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles OPTIONS requests for a file by returning a response with CORS headers.
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> OPTIONS_serveFile(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles GET requests for a file.
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param mime_type MIME type of the file
     * @param file_content Content of the file
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_serveFile(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, const std::string mime_type, const std::string & file_content);

    /**
     * @brief Handles GET requests for a binary file.
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param mime_type MIME type of the file
     * @param file_content Content of the file
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_serveBinaryFile(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, const std::string mime_type, const std::vector<std::byte> & file_content);

    /**
     * @brief Handle a GET request to /auth/nonce
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param auth_manager Auth manager instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_nonce(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, auth::AuthManager & auth_manager);


    /**
     * @brief Handles a OPTIONS request to /auth
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> OPTIONS_auth(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);
    
    /**
     * @brief Handles a POST request to /auth
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param auth_manager Auth manager instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> POST_auth(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, auth::AuthManager & auth_manager);
    

    /**
     * @brief Handles a OPTIONS request to /account
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> OPTIONS_accountInfo(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles a GET request to /account
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_accountInfo(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles a OPTIONS request to /formats?limit=<uint>&after=<~string>
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> OPTIONS_formats(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles a HEAD request to /formats?limit=<uint>&after=<~string>
     *
     * Mirrors GET /formats validation and returns status-only metadata.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> HEAD_formats(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles a GET request to /formats?limit=<uint>&after=<~string>
     *
     * Returns a paginated list of all format hashes.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_formats(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles a OPTIONS request to /format/<hash>?limit=<uint>&after=<~string>
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> OPTIONS_format(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles a GET request to /format/<hash>?limit=<uint>&after=<~string>
     *
     * Returns a paginated list of connectors that belong to the given format hash.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_format(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles a OPTIONS request to /accounts?limit=<uint>&after=<~string>
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> OPTIONS_accounts(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles a HEAD request to /accounts?limit=<uint>&after=<~string>
     *
     * Mirrors GET /accounts validation and returns status-only metadata.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> HEAD_accounts(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles a GET request to /accounts?limit=<uint>&after=<~string>
     *
     * Returns a paginated list of all unique account owners.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_accounts(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles OPTIONS requests to /feed?limit=<uint>&before=<~string>&type=<~string>&include_unfinalized=<~uint>
     */
    asio::awaitable<http::Response> OPTIONS_feed(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles GET requests to /feed?limit=<uint>&before=<~string>&type=<~string>&include_unfinalized=<~uint>
     */
    asio::awaitable<http::Response> GET_feed(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        feed::IFeedRepository & feed_repo);

    /**
     * @brief Handles OPTIONS requests to /feed/stream?since_seq=<~uint>&limit=<~uint>
     */
    asio::awaitable<http::Response> OPTIONS_feedStream(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Builds the SSE replay body for /feed/stream using the feed module.
     *
     * Returns the full SSE-framed string for the initial replay: a leading
     * `min_available_seq` comment, one `data:` frame per delta, and a trailing
     * `stream_meta` frame describing pagination state. Used by unit tests that
     * want a deterministic snapshot they can parse offline.
     */
    std::string buildFeedStreamSseReplay(
        feed::IFeedRepository & feed_repo,
        const feed::StreamQuery & query);

    /**
     * @brief Handles GET requests to /feed/stream?since_seq=<~uint>&limit=<~uint>
     *
     * Streaming handler — owns the socket for the request lifetime. Writes the
     * SSE response head, flushes an initial replay, then polls the feed
     * repository for new deltas and emits SSE frames until the client
     * disconnects, sending periodic `:keepalive` comments to keep the
     * connection (and the per-connection watchdog) alive when there are no new
     * events.
     */
    asio::awaitable<void> GET_feedStream(
        asio::ip::tcp::socket & sock,
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        std::chrono::steady_clock::time_point & deadline,
        feed::IFeedRepository & feed_repo);


    /**
     * @brief Handles HEAD requests for the connector endpoint.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance for retrieving connectors
     * @return An HTTP response
    */
    asio::awaitable<http::Response> HEAD_connector(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);
    
    /**
     * @brief Handles OPTIONS requests by returning a response with CORS headers.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response with CORS headers
     */
    asio::awaitable<http::Response> OPTIONS_connector(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handle a GET request to /connectors
     * 
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance for retrieving connectors
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_connector(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        registry::Registry & registry);

    /**
     * @brief Handle a POST request to /connectors
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance for retrieving connectors
     * @return An HTTP response
     */
    asio::awaitable<http::Response> POST_connector(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args,
        auth::AuthManager & auth_manager, registry::Registry & registry, evm::EVM & evm, const config::Config & config,
        const ChainAccess * chain = nullptr);

    /**
     * @brief Handles HEAD requests for the transformation endpoint.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance for retrieving transformations
     * @return An HTTP response
     */
    asio::awaitable<http::Response> HEAD_transformation(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles OPTIONS requests for the transformation endpoint by returning a response with CORS headers.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response with CORS headers for the transformation endpoint
     */
    asio::awaitable<http::Response> OPTIONS_transformation(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles GET requests for the transformation endpoint.
     *
     * Retrieves a transformation
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance for retrieving transformations
     * @param evm EVM instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_transformation(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        registry::Registry & registry);

    /**
     * @brief Handles POST requests for the transformation endpoint.
     *
     * Verifies the access token, then adds a new transformation to the registry.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param auth_manager Authentication manager instance for verifying access tokens
     * @param registry Registry instance for adding transformations
     * @param evm EVM instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> POST_transformation(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args,
        auth::AuthManager & auth_manager, registry::Registry & registry, evm::EVM & evm, const config::Config & config,
        const ChainAccess * chain = nullptr);


    /**
     * @brief Handles HEAD requests for the condition endpoint.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance for retrieving conditions
     * @return An HTTP response
     */
    asio::awaitable<http::Response> HEAD_condition(const http::Request & request,  std::vector<server::RouteArg> route_args, server::QueryArgsList query_args, registry::Registry & registry);

    /**
     * @brief Handles OPTIONS requests for the condition endpoint by returning a response with CORS headers.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @return An HTTP response
     */
    asio::awaitable<http::Response> OPTIONS_condition(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles GET requests for the condition endpoint.
     *
     * Retrieves a condition
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param registry Registry instance for retrieving conditions
     * @param evm EVM instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> GET_condition(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        registry::Registry & registry);

    /**
     * @brief Handles POST requests for the condition endpoint.
     *
     * Verifies the access token, then adds a new condition to the registry.
     *
     * @param request The incoming HTTP request
     * @param route_args Route arguments
     * @param query_args Query arguments
     * @param auth_manager Authentication manager instance for verifying access tokens
     * @param registry Registry instance for adding conditions
     * @param evm EVM instance
     * @return An HTTP response
     */
    asio::awaitable<http::Response> POST_condition(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args,
        auth::AuthManager & auth_manager, registry::Registry & registry, evm::EVM & evm, const config::Config & config,
        const ChainAccess * chain = nullptr);

    /**
     * @brief Handles OPTIONS requests for the /publish/* endpoints.
     */
    asio::awaitable<http::Response> OPTIONS_publish(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles POST /publish/<kind>/prepare, kind = transformation | condition | connector.
     *
     * Body {"name", "relay"?}. Rebuilds the caller's own local entity from its retained definition
     * and answers 200 with the unsigned registry transaction the owner's wallet sends; the
     * owner pays for it. With "relay": true the answer also
     * carries `signing` (account nonce, fees, type, value), which makes the transaction
     * signable offline for POST /publish/<kind>/send. Answers 200 with status
     * "published" when the registry already holds this exact publication, and 409 with
     * `missing`/`mismatched` names when a connector dependency is absent from, or
     * different on, the target registry. Dependencies are never published recursively.
     *
     * @param publisher Publisher bound to the configured deployment, or null when disabled
     */
    asio::awaitable<http::Response> POST_publishPrepare(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        auth::AuthManager & auth_manager,
        evm::EVM & evm,
        const publish::Publisher * publisher,
        const config::Config & config);

    /**
     * @brief Handles POST /publish/<kind>/send: broadcasts a publication the owner signed offline.
     *
     * Body {"name", "content_hash", "raw_tx"}, raw_tx being the signed type-2 transaction
     * built from a relay /prepare answer. The server checks that the owner signed it, that
     * it publishes exactly this entity on the configured registry, and that a dry run
     * does not revert, then forwards it through its own provider; the owner pays for it.
     * 202 {"status": "pending", "tx_hash"} once broadcast or already known to the
     * provider; confirm it with POST /publish/<kind>. 403 when another account signed it,
     * 409 when its account nonce is already used.
     *
     * @param publisher Publisher bound to the configured deployment, or null when disabled
     */
    asio::awaitable<http::Response> POST_publishSend(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        auth::AuthManager & auth_manager,
        const publish::Publisher * publisher);

    /**
     * @brief Handles POST /publish/<kind>: confirms the owner's publication transaction.
     *
     * Body {"name", "content_hash", "tx_hash"} with the content hash /prepare returned.
     * One receipt lookup: 201 when the transaction registered exactly this publication,
     * 202 while it is unmined (repeat the request), 400 when it reverted or registered
     * something else.
     *
     * @param publisher Publisher bound to the configured deployment, or null when disabled
     */
    asio::awaitable<http::Response> POST_publishConfirm(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        auth::AuthManager & auth_manager,
        const publish::Publisher * publisher);

    /**
     * @brief Handles OPTIONS requests for the execute and simulate endpoints.
     */
    asio::awaitable<http::Response> OPTIONS_execute(const http::Request & request, std::vector<server::RouteArg> route_args, server::QueryArgsList query_args);

    /**
     * @brief Handles POST /execute: runs a connector on chain.
     *
     * An eth_call of Runner.gen on the configured runner under the gas cap. The configured
     * block tag is resolved to one block and the call is pinned to that block's hash; the
     * answer is {"block_number", "block_hash", "runner", "particles"}, so anyone can check
     * it by calling the runner at the same block. The chain decides whether the connector
     * exists: 404 when the runner's registry does not hold it, 503 when this server has no
     * chain configured. No login is required: an eth_call sends no transaction and costs no gas.
     *
     * @param chain Configured chain deployment, or null when the server has no RPC endpoint
     */
    asio::awaitable<http::Response> POST_execute(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        const ChainAccess * chain);

    /**
     * @brief Handles POST /simulate: runs a registered connector on the local EVM.
     *
     * A connector that exists only on chain is first materialized in the local EVM with
     * its dependencies: each chain entity is replayed from the creation code the chain
     * deployed it from, as projected into the registry, so its code and constructor state
     * are the chain's. No chain is contacted and no compiler is involved. 503, naming the
     * entity and the reason, while a chain entity has no creation code (its projection is
     * incomplete, or its publication could not be recovered). 404 for a connector the
     * registry does not hold. No login is required.
     */
    asio::awaitable<http::Response> POST_simulate(
        const http::Request & request,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        registry::Registry & registry,
        evm::EVM & evm,
        const config::Config & config);
}
