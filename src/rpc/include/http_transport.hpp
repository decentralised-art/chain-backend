#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <asio.hpp>

#include "json_rpc.hpp"

namespace dcn::rpc
{
    // An HTTP endpoint parsed out of a provider or gateway URL. The URL commonly embeds an
    // API key in its path or query, so only redacted() may be logged.
    struct Endpoint
    {
        bool tls = true;
        std::string host;       // without IPv6 brackets
        std::string port;
        std::string target = "/";

        std::string redacted() const;
        std::string hostHeader() const;
    };

    // Accepts http:// and https:// URLs without userinfo; the scheme is case-insensitive.
    std::optional<Endpoint> parseEndpoint(std::string_view url);

    struct HttpTransportConfig
    {
        // Covers resolve, connect, TLS handshake, request write and response read.
        std::chrono::milliseconds request_timeout{20'000};
        std::size_t max_response_bytes = 32ull * 1024ull * 1024ull;
        std::size_t max_idle_connections = 4;

        // Overrides the system trust store when set. Empty means "use the platform roots".
        std::string ca_file;
        std::string ca_path;
    };

    struct HttpResponse
    {
        int status = 0;
        std::string body;
    };

    // HTTP/1.1 client for one endpoint. Every request owns its connection for its whole
    // duration, so concurrent requests never share a stream; healthy keep-alive
    // connections are pooled between requests. TLS verifies the certificate chain and
    // host name. All calls must come from the io_context's single thread.
    class IHttpClient
    {
        public:
            virtual ~IHttpClient() = default;

            virtual asio::awaitable<std::expected<HttpResponse, RpcError>> request(
                std::string_view method,
                std::string target,
                std::string body,
                std::string_view content_type = "application/json") = 0;

            virtual const Endpoint & endpoint() const = 0;

            // Aborts every in-flight request and fails any later one.
            virtual void cancel() = 0;
    };

    std::shared_ptr<IHttpClient> makeHttpClient(
        asio::io_context & io_context,
        Endpoint endpoint,
        HttpTransportConfig config = {});

    // JSON-RPC over HTTP POST to the URL's target. Returns nullptr when the URL cannot be
    // parsed. Never logs the URL itself.
    std::shared_ptr<ITransport> makeHttpTransport(
        asio::io_context & io_context,
        const std::string & url,
        HttpTransportConfig config = {});
}
