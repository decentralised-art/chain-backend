#include "http_transport.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <set>
#include <string_view>
#include <vector>

#include <asio/ssl.hpp>
#include <spdlog/spdlog.h>

#include "utils.hpp"

#if defined(_WIN32)
    #include <windows.h>
    #include <wincrypt.h>
    // wincrypt.h defines these as macros and breaks the OpenSSL declarations of the
    // same names that asio/ssl.hpp has already pulled in.
    #undef X509_NAME
    #undef X509_EXTENSIONS
    #undef PKCS7_SIGNER_INFO
    #undef OCSP_REQUEST
    #undef OCSP_RESPONSE
    #include <openssl/x509.h>
#endif

namespace dcn::rpc
{
    namespace
    {
        constexpr std::size_t MAX_HEADER_BYTES = 64 * 1024;

        RpcError _transportError(std::string message)
        {
            return RpcError{.kind = RpcError::Kind::TRANSPORT, .code = 0, .message = std::move(message)};
        }

        RpcError _fromEc(const std::error_code & ec, const std::string_view stage)
        {
            return _transportError(std::format("{} failed: {}", stage, ec.message()));
        }

        std::string _bracketed(const std::string & host)
        {
            return host.find(':') != std::string::npos ? std::format("[{}]", host) : host;
        }

        bool _isIpLiteral(const std::string & host)
        {
            std::error_code ec;
            (void)asio::ip::make_address(host, ec);
            return !ec;
        }

        // Populates the verification store with the platform roots. asio's
        // set_default_verify_paths() finds nothing on Windows, which would make
        // verify_peer fail for every provider, so the system store is imported there.
        bool _loadSystemRoots(asio::ssl::context & context)
        {
#if defined(_WIN32)
            HCERTSTORE store = ::CertOpenSystemStoreW(0, L"ROOT");
            if(store == nullptr)
            {
                return false;
            }

            X509_STORE * x509_store = ::SSL_CTX_get_cert_store(context.native_handle());
            std::size_t loaded = 0;
            PCCERT_CONTEXT cert = nullptr;
            while((cert = ::CertEnumCertificatesInStore(store, cert)) != nullptr)
            {
                const unsigned char * encoded = cert->pbCertEncoded;
                X509 * x509 = ::d2i_X509(nullptr, &encoded, static_cast<long>(cert->cbCertEncoded));
                if(x509 == nullptr)
                {
                    continue;
                }
                if(::X509_STORE_add_cert(x509_store, x509) == 1)
                {
                    ++loaded;
                }
                ::X509_free(x509);
            }

            ::CertCloseStore(store, 0);
            return loaded > 0;
#else
            std::error_code ec;
            context.set_default_verify_paths(ec);
            return !ec;
#endif
        }

        using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;

        // One TCP (optionally TLS) connection. Owned by exactly one request at a time.
        class Connection
        {
            public:
                Connection(const asio::any_io_executor & executor, asio::ssl::context * tls_context)
                {
                    if(tls_context != nullptr)
                    {
                        _tls = std::make_unique<TlsStream>(executor, *tls_context);
                    }
                    else
                    {
                        _plain = std::make_unique<asio::ip::tcp::socket>(executor);
                    }
                }

                bool tls() const { return _tls != nullptr; }
                TlsStream & tlsStream() { return *_tls; }
                asio::ip::tcp::socket & plainSocket() { return *_plain; }
                asio::ip::tcp::socket & socket() { return _tls ? _tls->next_layer() : *_plain; }

                // Aborts any pending operation on this connection. The stream objects stay
                // alive, so an operation still referencing them completes safely.
                void close()
                {
                    std::error_code ec;
                    (void)socket().close(ec);
                    reusable = false;
                }

                bool reusable = false;

            private:
                std::unique_ptr<TlsStream> _tls;
                std::unique_ptr<asio::ip::tcp::socket> _plain;
        };

        // Per-request state shared with the deadline handler, which can run after the
        // request coroutine has moved on.
        struct RequestState
        {
            explicit RequestState(const asio::any_io_executor & executor)
                : resolver(executor)
            {
            }

            void abort(RpcError reason)
            {
                if(aborted || finished) return;
                aborted = true;
                abort_reason = std::move(reason);
                resolver.cancel();
                if(connection) connection->close();
            }

            asio::ip::tcp::resolver resolver;
            std::shared_ptr<Connection> connection;
            bool aborted = false;
            bool finished = false;
            RpcError abort_reason;
        };

        asio::awaitable<std::size_t> _readUntil(
            Connection & connection,
            asio::streambuf & buffer,
            const std::string_view delimiter,
            std::error_code & ec)
        {
            ec.clear();
            if(connection.tls())
            {
                co_return co_await asio::async_read_until(
                    connection.tlsStream(), buffer, delimiter, asio::redirect_error(asio::use_awaitable, ec));
            }
            co_return co_await asio::async_read_until(
                connection.plainSocket(), buffer, delimiter, asio::redirect_error(asio::use_awaitable, ec));
        }

        asio::awaitable<void> _readAtLeast(
            Connection & connection,
            asio::streambuf & buffer,
            const std::size_t bytes,
            std::error_code & ec)
        {
            ec.clear();
            if(buffer.size() >= bytes)
            {
                co_return;
            }
            const std::size_t missing = bytes - buffer.size();
            if(connection.tls())
            {
                (void)co_await asio::async_read(
                    connection.tlsStream(), buffer, asio::transfer_exactly(missing),
                    asio::redirect_error(asio::use_awaitable, ec));
            }
            else
            {
                (void)co_await asio::async_read(
                    connection.plainSocket(), buffer, asio::transfer_exactly(missing),
                    asio::redirect_error(asio::use_awaitable, ec));
            }
        }

        // Reads until the peer closes the connection. Returns true when the buffer filled
        // up before the peer closed, i.e. the body exceeds the configured limit.
        asio::awaitable<bool> _readToEof(Connection & connection, asio::streambuf & buffer, std::error_code & ec)
        {
            ec.clear();
            if(connection.tls())
            {
                (void)co_await asio::async_read(
                    connection.tlsStream(), buffer, asio::transfer_all(), asio::redirect_error(asio::use_awaitable, ec));
            }
            else
            {
                (void)co_await asio::async_read(
                    connection.plainSocket(), buffer, asio::transfer_all(), asio::redirect_error(asio::use_awaitable, ec));
            }

            if(ec == asio::error::eof || ec == asio::ssl::error::stream_truncated)
            {
                ec.clear();
                co_return false;
            }
            co_return !ec;
        }

        std::string _take(asio::streambuf & buffer, const std::size_t bytes)
        {
            std::string out(
                asio::buffers_begin(buffer.data()),
                asio::buffers_begin(buffer.data()) + static_cast<std::ptrdiff_t>(bytes));
            buffer.consume(bytes);
            return out;
        }

        struct ResponseHead
        {
            int status = 0;
            bool keep_alive = true;
            bool chunked = false;
            std::optional<std::size_t> content_length;
        };

        std::expected<ResponseHead, RpcError> _parseHead(const std::string & headers)
        {
            ResponseHead head;

            const std::size_t status_end = headers.find("\r\n");
            const std::string_view status_line(headers.data(), status_end == std::string::npos ? headers.size() : status_end);
            if(!status_line.starts_with("HTTP/1."))
            {
                return std::unexpected(RpcError::protocol("malformed HTTP status line"));
            }
            head.keep_alive = !status_line.starts_with("HTTP/1.0");

            const std::size_t first_space = status_line.find(' ');
            if(first_space == std::string_view::npos || status_line.size() < first_space + 4)
            {
                return std::unexpected(RpcError::protocol("malformed HTTP status line"));
            }
            const std::string_view code_text = status_line.substr(first_space + 1, 3);
            const auto [code_end, code_ec] = std::from_chars(code_text.data(), code_text.data() + code_text.size(), head.status);
            if(code_ec != std::errc{} || code_end != code_text.data() + code_text.size() || head.status < 100)
            {
                return std::unexpected(RpcError::protocol("malformed HTTP status code"));
            }

            std::size_t line_begin = status_end == std::string::npos ? headers.size() : status_end + 2;
            while(line_begin < headers.size())
            {
                const std::size_t line_end = headers.find("\r\n", line_begin);
                if(line_end == std::string::npos || line_end == line_begin)
                {
                    break;
                }

                const std::string_view line(headers.data() + line_begin, line_end - line_begin);
                line_begin = line_end + 2;

                const std::size_t colon = line.find(':');
                if(colon == std::string_view::npos)
                {
                    continue;
                }

                const std::string name = utils::toLower(std::string(utils::trimView(line.substr(0, colon))));
                const std::string value = utils::toLower(std::string(utils::trimView(line.substr(colon + 1))));

                if(name == "content-length")
                {
                    std::size_t parsed = 0;
                    const auto [end, parse_ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                    if(parse_ec != std::errc{} || end != value.data() + value.size()
                       || (head.content_length && *head.content_length != parsed))
                    {
                        return std::unexpected(RpcError::protocol("malformed Content-Length"));
                    }
                    head.content_length = parsed;
                }
                else if(name == "transfer-encoding" && value.find("chunked") != std::string::npos)
                {
                    head.chunked = true;
                }
                else if(name == "connection")
                {
                    if(value.find("close") != std::string::npos) head.keep_alive = false;
                    else if(value.find("keep-alive") != std::string::npos) head.keep_alive = true;
                }
            }

            return head;
        }

        class HttpClient final : public IHttpClient, public std::enable_shared_from_this<HttpClient>
        {
            public:
                HttpClient(asio::io_context & io_context, Endpoint endpoint, HttpTransportConfig config)
                    : _io_context(io_context)
                    , _endpoint(std::move(endpoint))
                    , _config(std::move(config))
                    , _ssl_context(asio::ssl::context::tls_client)
                {
                    if(!_endpoint.tls)
                    {
                        return;
                    }

                    _ssl_context.set_options(
                        asio::ssl::context::default_workarounds |
                        asio::ssl::context::no_sslv2 |
                        asio::ssl::context::no_sslv3 |
                        asio::ssl::context::no_tlsv1 |
                        asio::ssl::context::no_tlsv1_1 |
                        asio::ssl::context::single_dh_use);

                    std::error_code ec;
                    bool roots_loaded = false;
                    if(!_config.ca_file.empty())
                    {
                        _ssl_context.load_verify_file(_config.ca_file, ec);
                        roots_loaded = !ec;
                    }
                    else if(!_config.ca_path.empty())
                    {
                        _ssl_context.add_verify_path(_config.ca_path, ec);
                        roots_loaded = !ec;
                    }
                    else
                    {
                        roots_loaded = _loadSystemRoots(_ssl_context);
                    }

                    if(!roots_loaded)
                    {
                        spdlog::error(
                            "http {}: no trusted CA roots available; TLS verification will reject every connection",
                            _endpoint.redacted());
                    }

                    _ssl_context.set_verify_mode(asio::ssl::verify_peer);
                    _ssl_context.set_verify_callback(asio::ssl::host_name_verification(_endpoint.host));
                }

                asio::awaitable<std::expected<HttpResponse, RpcError>> request(
                    std::string_view method,
                    std::string target,
                    std::string body,
                    std::string_view content_type) override;

                const Endpoint & endpoint() const override
                {
                    return _endpoint;
                }

                void cancel() override
                {
                    _cancelled = true;
                    for(const auto & state : _active)
                    {
                        state->abort(_transportError("request cancelled"));
                    }
                    for(const auto & connection : _idle)
                    {
                        connection->close();
                    }
                    _idle.clear();
                }

            private:
                asio::awaitable<std::expected<std::shared_ptr<Connection>, RpcError>> _connect(RequestState & state);

                asio::awaitable<std::expected<HttpResponse, RpcError>> _readResponse(
                    RequestState & state,
                    Connection & connection,
                    asio::streambuf & buffer);

                asio::awaitable<std::expected<HttpResponse, RpcError>> _exchange(
                    RequestState & state,
                    Connection & connection,
                    const std::string & request);

                std::shared_ptr<Connection> _takeIdle()
                {
                    if(_idle.empty()) return nullptr;
                    auto connection = std::move(_idle.back());
                    _idle.pop_back();
                    return connection;
                }

                void _returnIdle(std::shared_ptr<Connection> connection)
                {
                    if(_cancelled || _idle.size() >= _config.max_idle_connections)
                    {
                        connection->close();
                        return;
                    }
                    _idle.push_back(std::move(connection));
                }

                asio::io_context & _io_context;
                Endpoint _endpoint;
                HttpTransportConfig _config;
                asio::ssl::context _ssl_context;
                std::vector<std::shared_ptr<Connection>> _idle;
                std::set<std::shared_ptr<RequestState>> _active;
                bool _cancelled = false;
        };

        asio::awaitable<std::expected<std::shared_ptr<Connection>, RpcError>> HttpClient::_connect(RequestState & state)
        {
            std::error_code ec;
            const auto endpoints = co_await state.resolver.async_resolve(
                _endpoint.host, _endpoint.port, asio::redirect_error(asio::use_awaitable, ec));
            if(state.aborted) co_return std::unexpected(state.abort_reason);
            if(ec) co_return std::unexpected(_fromEc(ec, "resolve"));

            auto connection = std::make_shared<Connection>(
                _io_context.get_executor(), _endpoint.tls ? &_ssl_context : nullptr);
            state.connection = connection;

            // SNI carries host names only; an IP literal is verified against the
            // certificate's IP SANs instead.
            if(connection->tls() && !_isIpLiteral(_endpoint.host)
               && ::SSL_set_tlsext_host_name(connection->tlsStream().native_handle(), _endpoint.host.c_str()) != 1)
            {
                co_return std::unexpected(_transportError("failed to set TLS SNI host"));
            }

            co_await asio::async_connect(connection->socket(), endpoints, asio::redirect_error(asio::use_awaitable, ec));
            if(state.aborted) co_return std::unexpected(state.abort_reason);
            if(ec) co_return std::unexpected(_fromEc(ec, "connect"));

            std::error_code option_ec;
            (void)connection->socket().set_option(asio::ip::tcp::no_delay(true), option_ec);

            if(connection->tls())
            {
                co_await connection->tlsStream().async_handshake(
                    asio::ssl::stream_base::client, asio::redirect_error(asio::use_awaitable, ec));
                if(state.aborted) co_return std::unexpected(state.abort_reason);
                if(ec) co_return std::unexpected(_fromEc(ec, "TLS handshake"));
            }

            co_return connection;
        }

        asio::awaitable<std::expected<HttpResponse, RpcError>> HttpClient::_readResponse(
            RequestState & state,
            Connection & connection,
            asio::streambuf & buffer)
        {
            std::error_code ec;

            const std::size_t header_size = co_await _readUntil(connection, buffer, "\r\n\r\n", ec);
            if(state.aborted) co_return std::unexpected(state.abort_reason);
            if(ec) co_return std::unexpected(_fromEc(ec, "response header read"));
            if(header_size > MAX_HEADER_BYTES)
            {
                co_return std::unexpected(RpcError::protocol("response headers exceed size limit"));
            }

            const auto head = _parseHead(_take(buffer, header_size));
            if(!head) co_return std::unexpected(head.error());

            HttpResponse response{.status = head->status};
            bool keep_alive = head->keep_alive;

            if(head->chunked)
            {
                while(true)
                {
                    const std::size_t line_size = co_await _readUntil(connection, buffer, "\r\n", ec);
                    if(state.aborted) co_return std::unexpected(state.abort_reason);
                    if(ec) co_return std::unexpected(_fromEc(ec, "chunk size read"));

                    const std::string size_line = _take(buffer, line_size);
                    const std::string_view trimmed = utils::trimView(size_line);
                    const std::string_view size_text = trimmed.substr(0, trimmed.find(';'));
                    std::size_t chunk_size = 0;
                    const auto [end, parse_ec] = std::from_chars(
                        size_text.data(), size_text.data() + size_text.size(), chunk_size, 16);
                    if(size_text.empty() || parse_ec != std::errc{} || end != size_text.data() + size_text.size())
                    {
                        co_return std::unexpected(RpcError::protocol("malformed chunk size"));
                    }

                    if(chunk_size == 0)
                    {
                        break;
                    }

                    if(chunk_size > _config.max_response_bytes - response.body.size())
                    {
                        co_return std::unexpected(RpcError{
                            .kind = RpcError::Kind::LIMIT, .code = 0, .message = "response exceeds configured size limit"});
                    }

                    co_await _readAtLeast(connection, buffer, chunk_size + 2, ec);
                    if(state.aborted) co_return std::unexpected(state.abort_reason);
                    if(ec) co_return std::unexpected(_fromEc(ec, "chunk body read"));

                    std::string chunk = _take(buffer, chunk_size + 2);
                    if(!chunk.ends_with("\r\n"))
                    {
                        co_return std::unexpected(RpcError::protocol("chunk is not terminated by CRLF"));
                    }
                    chunk.resize(chunk_size);
                    response.body += chunk;
                }

                // Optional trailer fields, terminated by an empty line.
                std::size_t trailer_bytes = 0;
                while(true)
                {
                    const std::size_t line_size = co_await _readUntil(connection, buffer, "\r\n", ec);
                    if(state.aborted) co_return std::unexpected(state.abort_reason);
                    if(ec) co_return std::unexpected(_fromEc(ec, "chunk trailer read"));

                    buffer.consume(line_size);
                    if(line_size == 2)
                    {
                        break;
                    }
                    trailer_bytes += line_size;
                    if(trailer_bytes > MAX_HEADER_BYTES)
                    {
                        co_return std::unexpected(RpcError::protocol("chunk trailer exceeds size limit"));
                    }
                }
            }
            else if(head->content_length)
            {
                if(*head->content_length > _config.max_response_bytes)
                {
                    co_return std::unexpected(RpcError{
                        .kind = RpcError::Kind::LIMIT, .code = 0, .message = "response exceeds configured size limit"});
                }

                co_await _readAtLeast(connection, buffer, *head->content_length, ec);
                if(state.aborted) co_return std::unexpected(state.abort_reason);
                if(ec) co_return std::unexpected(_fromEc(ec, "response body read"));

                response.body = _take(buffer, *head->content_length);
            }
            else
            {
                // No framing: the body runs until the peer closes the connection.
                keep_alive = false;
                const bool overflow = co_await _readToEof(connection, buffer, ec);
                if(state.aborted) co_return std::unexpected(state.abort_reason);
                if(ec) co_return std::unexpected(_fromEc(ec, "response body read"));
                if(overflow || buffer.size() > _config.max_response_bytes)
                {
                    co_return std::unexpected(RpcError{
                        .kind = RpcError::Kind::LIMIT, .code = 0, .message = "response exceeds configured size limit"});
                }
                response.body = _take(buffer, buffer.size());
            }

            // Bytes beyond the response would belong to nothing we asked for.
            connection.reusable = keep_alive && buffer.size() == 0;
            co_return response;
        }

        asio::awaitable<std::expected<HttpResponse, RpcError>> HttpClient::_exchange(
            RequestState & state,
            Connection & connection,
            const std::string & request)
        {
            std::error_code ec;
            if(connection.tls())
            {
                co_await asio::async_write(
                    connection.tlsStream(), asio::buffer(request), asio::redirect_error(asio::use_awaitable, ec));
            }
            else
            {
                co_await asio::async_write(
                    connection.plainSocket(), asio::buffer(request), asio::redirect_error(asio::use_awaitable, ec));
            }

            if(state.aborted) co_return std::unexpected(state.abort_reason);
            if(ec) co_return std::unexpected(_fromEc(ec, "request write"));

            asio::streambuf buffer(_config.max_response_bytes + MAX_HEADER_BYTES + 1024);
            co_return co_await _readResponse(state, connection, buffer);
        }

        asio::awaitable<std::expected<HttpResponse, RpcError>> HttpClient::request(
            const std::string_view method,
            std::string target,
            std::string body,
            const std::string_view content_type)
        {
            if(_cancelled)
            {
                co_return std::unexpected(_transportError("HTTP client is shut down"));
            }

            // Keeps the client, its TLS context and its pool alive for the whole request.
            const auto self = shared_from_this();

            auto state = std::make_shared<RequestState>(_io_context.get_executor());
            _active.insert(state);

            // One deadline covers resolve, connect, handshake, write and read.
            asio::steady_timer deadline(_io_context);
            deadline.expires_after(_config.request_timeout);
            deadline.async_wait([state](const std::error_code & ec)
            {
                if(ec) return;
                state->abort(RpcError{
                    .kind = RpcError::Kind::TIMEOUT, .code = 0, .message = "request deadline elapsed"});
            });

            std::string request_text = std::format(
                "{} {} HTTP/1.1\r\n"
                "Host: {}\r\n"
                "Accept: application/json\r\n"
                "Connection: keep-alive\r\n",
                method,
                target,
                _endpoint.hostHeader());
            if(!body.empty() || method == "POST")
            {
                request_text += std::format("Content-Type: {}\r\nContent-Length: {}\r\n", content_type, body.size());
            }
            request_text += "\r\n";
            request_text += body;

            std::expected<HttpResponse, RpcError> result = std::unexpected(_transportError("request not attempted"));
            for(int attempt = 0; attempt < 2; ++attempt)
            {
                std::shared_ptr<Connection> connection = _takeIdle();
                const bool reused = connection != nullptr;
                if(!connection)
                {
                    auto connected = co_await _connect(*state);
                    if(!connected)
                    {
                        result = std::unexpected(connected.error());
                        break;
                    }
                    connection = std::move(*connected);
                }
                state->connection = connection;

                result = co_await _exchange(*state, *connection, request_text);
                if(result)
                {
                    break;
                }

                connection->close();
                state->connection.reset();

                // A pooled keep-alive connection may have been closed by the provider while
                // idle; one fresh connection covers that without masking a genuine failure.
                if(!reused || state->aborted || result.error().kind != RpcError::Kind::TRANSPORT)
                {
                    break;
                }
            }

            state->finished = true;
            deadline.cancel();
            _active.erase(state);

            if(state->aborted)
            {
                if(state->connection) state->connection->close();
                co_return std::unexpected(state->abort_reason);
            }

            if(result && state->connection && state->connection->reusable)
            {
                _returnIdle(std::move(state->connection));
            }

            co_return result;
        }

        // Maps HTTP status to the JSON-RPC error model. Several providers answer a
        // JSON-RPC error with a 4xx status; the error object decides the classification.
        std::expected<std::string, RpcError> _interpretJsonRpcResponse(HttpResponse response)
        {
            const int status = response.status;
            if(status >= 200 && status < 300)
            {
                return std::move(response.body);
            }
            if(status == 429)
            {
                if(mentionsQuotaExhausted(response.body))
                {
                    return std::unexpected(RpcError{
                        .kind = RpcError::Kind::QUOTA, .code = status, .message = "provider usage quota exhausted"});
                }
                return std::unexpected(RpcError{
                    .kind = RpcError::Kind::THROTTLED, .code = status, .message = "provider rate limited request"});
            }
            if(status == 408)
            {
                return std::unexpected(RpcError{
                    .kind = RpcError::Kind::TIMEOUT, .code = status, .message = "provider timed out the request"});
            }
            if(status == 413)
            {
                return std::unexpected(RpcError{
                    .kind = RpcError::Kind::LIMIT, .code = status, .message = "provider rejected request as too large"});
            }
            if(status >= 500)
            {
                return std::unexpected(RpcError{
                    .kind = RpcError::Kind::TRANSPORT,
                    .code = status,
                    .message = std::format("provider returned HTTP {}", status)});
            }

            const json parsed = json::parse(response.body, nullptr, false);
            if(parsed.is_object() && parsed.contains("error"))
            {
                return std::move(response.body);
            }

            return std::unexpected(RpcError{
                .kind = RpcError::Kind::PROTOCOL,
                .code = status,
                .message = std::format("provider returned HTTP {}", status)});
        }

        class JsonRpcHttpTransport final : public ITransport
        {
            public:
                explicit JsonRpcHttpTransport(std::shared_ptr<IHttpClient> http)
                    : _http(std::move(http))
                    , _description(_http->endpoint().redacted())
                {
                }

                asio::awaitable<std::expected<std::string, RpcError>> post(std::string body) override
                {
                    auto response = co_await _http->request("POST", _http->endpoint().target, std::move(body));
                    if(!response) co_return std::unexpected(response.error());
                    co_return _interpretJsonRpcResponse(std::move(*response));
                }

                std::string_view describe() const override
                {
                    return _description;
                }

                void cancel() override
                {
                    _http->cancel();
                }

            private:
                std::shared_ptr<IHttpClient> _http;
                std::string _description;
        };
    }

    std::string Endpoint::redacted() const
    {
        return std::format("{}://{}:{}/<redacted>", tls ? "https" : "http", _bracketed(host), port);
    }

    std::string Endpoint::hostHeader() const
    {
        const bool default_port = (tls && port == "443") || (!tls && port == "80");
        return default_port ? _bracketed(host) : std::format("{}:{}", _bracketed(host), port);
    }

    std::optional<Endpoint> parseEndpoint(const std::string_view url)
    {
        Endpoint endpoint;

        std::string_view rest;
        if(utils::equalsIgnoreCase(url.substr(0, 8), "https://"))
        {
            endpoint.tls = true;
            rest = url.substr(8);
        }
        else if(utils::equalsIgnoreCase(url.substr(0, 7), "http://"))
        {
            endpoint.tls = false;
            rest = url.substr(7);
        }
        else
        {
            return std::nullopt;
        }

        const std::size_t authority_end = rest.find_first_of("/?#");
        const std::string_view authority = rest.substr(0, authority_end);
        std::string_view remainder = authority_end == std::string_view::npos
            ? std::string_view{}
            : rest.substr(authority_end);
        if(const std::size_t fragment = remainder.find('#'); fragment != std::string_view::npos)
        {
            remainder = remainder.substr(0, fragment);
        }
        if(remainder.empty())
        {
            endpoint.target = "/";
        }
        else if(remainder.front() == '/')
        {
            endpoint.target = std::string(remainder);
        }
        else
        {
            endpoint.target = std::format("/{}", remainder);
        }

        // Userinfo in an RPC URL is a credential; reject rather than forward it.
        if(authority.empty() || authority.find('@') != std::string_view::npos)
        {
            return std::nullopt;
        }

        std::string_view host;
        std::string_view port;
        if(authority.front() == '[')
        {
            const std::size_t close = authority.find(']');
            if(close == std::string_view::npos)
            {
                return std::nullopt;
            }
            host = authority.substr(1, close - 1);
            const std::string_view after = authority.substr(close + 1);
            if(!after.empty())
            {
                if(after.front() != ':' || after.size() == 1)
                {
                    return std::nullopt;
                }
                port = after.substr(1);
            }
        }
        else
        {
            const std::size_t colon = authority.rfind(':');
            if(colon != std::string_view::npos)
            {
                host = authority.substr(0, colon);
                port = authority.substr(colon + 1);
                if(port.empty())
                {
                    return std::nullopt;
                }
            }
            else
            {
                host = authority;
            }
        }

        if(host.empty())
        {
            return std::nullopt;
        }

        if(port.empty())
        {
            endpoint.port = endpoint.tls ? "443" : "80";
        }
        else
        {
            unsigned int value = 0;
            const auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
            if(ec != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535)
            {
                return std::nullopt;
            }
            endpoint.port = std::to_string(value);
        }

        endpoint.host = std::string(host);
        return endpoint;
    }

    std::shared_ptr<IHttpClient> makeHttpClient(
        asio::io_context & io_context,
        Endpoint endpoint,
        HttpTransportConfig config)
    {
        return std::make_shared<HttpClient>(io_context, std::move(endpoint), std::move(config));
    }

    std::shared_ptr<ITransport> makeHttpTransport(
        asio::io_context & io_context,
        const std::string & url,
        HttpTransportConfig config)
    {
        auto endpoint = parseEndpoint(url);
        if(!endpoint)
        {
            spdlog::error("rpc: unusable endpoint URL (must be http:// or https:// without userinfo)");
            return nullptr;
        }

        return std::make_shared<JsonRpcHttpTransport>(
            makeHttpClient(io_context, std::move(*endpoint), std::move(config)));
    }
}
