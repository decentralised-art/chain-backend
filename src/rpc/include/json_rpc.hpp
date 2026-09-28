#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <random>
#include <string>
#include <string_view>

#include <asio.hpp>
#include <nlohmann/json.hpp>

namespace dcn::rpc
{
    using json = nlohmann::json;

    struct RpcError
    {
        enum class Kind : std::uint8_t
        {
            TRANSPORT,  // socket, TLS or connection failure, or a transient provider fault
            TIMEOUT,    // connect/request deadline elapsed
            THROTTLED,  // provider rate limit
            LIMIT,      // response or block range too large for the provider
            PROTOCOL,   // malformed response, id mismatch, unparsable quantity
            EXECUTION   // deterministic provider error, including EVM reverts
        };

        Kind kind = Kind::TRANSPORT;
        int code = 0;
        std::string message;
        std::string data;

        // TRANSPORT/TIMEOUT/THROTTLED are worth retrying unchanged. LIMIT needs a
        // smaller request, so the caller decides. PROTOCOL/EXECUTION are deterministic.
        bool retryable() const;
    };

    std::string toString(RpcError::Kind kind);

    // One JSON-RPC request/response round trip. Implemented over HTTP in production and
    // by fakes in tests.
    class ITransport
    {
        public:
            virtual ~ITransport() = default;

            virtual asio::awaitable<std::expected<std::string, RpcError>> post(std::string body) = 0;

            // Redacted endpoint description, safe to log.
            virtual std::string_view describe() const = 0;

            // Aborts every in-flight request and fails any later one. Used on shutdown so a
            // slow provider cannot hold the process open.
            virtual void cancel() {}
    };

    struct JsonRpcConfig
    {
        std::size_t max_attempts = 4;
        std::uint64_t initial_backoff_ms = 200;
        std::uint64_t max_backoff_ms = 5000;
    };

    // Provider-neutral JSON-RPC 2.0 client: matches response ids, classifies provider
    // errors, and retries only transport failures and explicit throttling.
    class JsonRpcClient
    {
        public:
            JsonRpcClient(std::shared_ptr<ITransport> transport, JsonRpcConfig config = {});

            // `retry_timeouts` is false for requests whose timeout means "ask for less"
            // rather than "ask again", such as a wide eth_getLogs range.
            asio::awaitable<std::expected<json, RpcError>> call(
                std::string_view method,
                json params,
                bool retry_timeouts = true);

            std::string_view endpoint() const;

            void cancel();

        private:
            asio::awaitable<std::expected<json, RpcError>> _callOnce(std::string_view method, const json & params);
            std::uint64_t _backoffMs(std::size_t attempt);

            std::shared_ptr<ITransport> _transport;
            JsonRpcConfig _config;
            std::uint64_t _next_id = 1;
            std::minstd_rand _jitter;
            bool _cancelled = false;
    };

    // Classifies the `error` member of a JSON-RPC response. Exposed for tests.
    RpcError classifyProviderError(const json & error);
}
