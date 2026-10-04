#include <exception>
#include <format>
#include <string_view>

#include "server.hpp"
#include "utils.hpp"

namespace dcn::server
{
    namespace
    {
        http::Response sizeRejection(const http::Code code)
        {
            http::Response response;
            response.setVersion("HTTP/1.1");
            response.setCode(code);
            response.setHeader(http::Header::Connection, "close");
            response.setBodyWithContentLength(std::format("{}", code));
            return response;
        }
    }

    std::optional<http::Code> requestSizeRejection(
        const std::size_t received,
        const bool headers_complete,
        const std::size_t content_length,
        const std::size_t max_body_size)
    {
        if(!headers_complete)
        {
            if(received > MAX_HEADER_SIZE) return http::Code::RequestHeaderFieldsTooLarge;
            return std::nullopt;
        }
        if(content_length > max_body_size) return http::Code::PayloadTooLarge;
        return std::nullopt;
    }

    Server::Server(asio::io_context & io_context, asio::ip::tcp::endpoint endpoint)
    :   _io_context(io_context),
        _strand(asio::make_strand(io_context)),
        _close(false),
        _acceptor(_strand, std::move(endpoint)),
        _idle_interval(std::chrono::milliseconds(5000))
    {
    }

    asio::awaitable<void> Server::close()
    {
        co_await asio::dispatch(asio::bind_executor(_strand, asio::use_awaitable));
        spdlog::debug("decentralised.art chain backend request close");
        _close = true;
        co_return;
    }

    asio::awaitable<void> Server::listen()
    {
        co_await asio::dispatch(asio::bind_executor(_strand, asio::use_awaitable));
        spdlog::info("decentralised.art chain backend listening on port {}", _acceptor.local_endpoint().port());
        std::chrono::steady_clock::time_point listen_deadline{};

        while (_close == false)
        {
            listen_deadline = std::chrono::steady_clock::now() + _idle_interval;
            auto socket_result = co_await (_acceptor.async_accept(asio::use_awaitable) || async::watchdog(listen_deadline));
            if(std::holds_alternative<asio::ip::tcp::socket>(socket_result))
            {
                // spawn handleConnection to _io_context - not use server strand
                asio::co_spawn(
                    _io_context,
                    handleConnection(std::move(std::get<asio::ip::tcp::socket>(socket_result))),
                    [](std::exception_ptr exception_ptr)
                    {
                        utils::logException(exception_ptr, "Connection coroutine failed");
                    });
            }
        }
        co_return;
    }

    void Server::setIdleInterval(std::chrono::milliseconds idle_interval)
    {
        _idle_interval = idle_interval;
    }

    void Server::setMaxBodySize(const std::size_t max_body_size)
    {
        _max_body_size = max_body_size;
    }

    asio::awaitable<void> Server::handleConnection(asio::ip::tcp::socket sock)
    {
        spdlog::info("New connection started");
        std::chrono::steady_clock::time_point deadline{};

        try
        {
            // read data
            co_await (readData(sock, deadline) || async::watchdog(deadline));
        }
        catch(const std::exception & e)
        {
            spdlog::error("Connection terminated with exception: {}", e.what());
        }
        catch(...)
        {
            spdlog::error("Connection terminated with unknown exception");
        }

        spdlog::info("Connection ended");
        co_return;
    }

    asio::awaitable<void> Server::readData(asio::ip::tcp::socket & sock, std::chrono::steady_clock::time_point & deadline)
    {
        std::size_t bytes_transferred = 0;
        std::array<char, 8192> read_buffer;

        std::string full_request_data;

        http::Request request;
        http::Response response;

        while(_close == false)
        {
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            try
            {
                bytes_transferred = co_await sock.async_read_some(asio::buffer(read_buffer), asio::use_awaitable);
            }
            catch(...)
            {
                spdlog::debug("client disconnected");
                co_return;
            }

            if(bytes_transferred == 0)co_return;
            
            full_request_data.append(read_buffer.data(), bytes_transferred);

            // Try to find end of headers
            std::size_t header_end_pos = full_request_data.find("\r\n\r\n");
            if (header_end_pos == std::string::npos)
            {
                if(const auto rejection = requestSizeRejection(full_request_data.size(), false, 0, _max_body_size))
                {
                    spdlog::warn("Refusing a request whose headers exceed {} bytes", MAX_HEADER_SIZE);
                    co_await writeData(sock, std::format("{}", sizeRejection(*rejection)));
                    co_return;
                }
                continue; // keep reading until full headers are received
            }

            // Parse headers first
            request = parse::parseRequestFromString(full_request_data);
            const http::Request & const_request = request;

            // Determine expected body size
            std::size_t content_length = 0;
            const auto content_len_header =  request.getHeader(http::Header::ContentLength);
            if(content_len_header.size() == 1)
            {
                try
                {
                    content_length = std::stoul(content_len_header.at(0));
                }
                catch(...)
                {
                    spdlog::error("Invalid content length header");
                    co_return;
                }

                // Refused before the body is read, so a declared size never has to fit in memory.
                if(const auto rejection = requestSizeRejection(header_end_pos + 4, true, content_length, _max_body_size))
                {
                    spdlog::warn("Refusing a {}-byte request body; the limit is {} bytes", content_length, _max_body_size);
                    co_await writeData(sock, std::format("{}", sizeRejection(*rejection)));
                    co_return;
                }

                std::size_t actual_body_start = header_end_pos + 4;
                std::size_t current_body_size = full_request_data.size() - actual_body_start;

                // Read the rest of the body if needed
                while (current_body_size < content_length)
                {
                    bytes_transferred = co_await sock.async_read_some(asio::buffer(read_buffer), asio::use_awaitable);
                    if (bytes_transferred == 0)
                    {
                        spdlog::warn("client disconnected while reading body");
                        co_return;
                    };

                    full_request_data.append(read_buffer.data(), bytes_transferred);
                    current_body_size += bytes_transferred;
                }

                // Update the request body after full body is read
                request.setBody(full_request_data.substr(actual_body_start, content_length));
            }

            // The request line only: headers and body carry access tokens and signatures.
            if(spdlog::should_log(spdlog::level::debug))
            {
                spdlog::debug(std::format("Received request: {} {}", request.getMethod(), request.getPath()));
            }

            auto [handler, route_args, query_args] = _router.findRoute(request);

            if (handler && handler->kind() == RouteHandlerFunc::Kind::Streaming)
            {
                // Streaming handler owns the socket for the full lifetime of this
                // request. It is responsible for writing status line + headers + body
                // and for refreshing `deadline` so the connection-level watchdog
                // does not trip during healthy streaming. When it returns we are
                // done with this connection.
                try
                {
                    co_await handler->invokeStreaming(sock, const_request, std::move(route_args), std::move(query_args), deadline);
                }
                catch(const std::exception & e)
                {
                    spdlog::error("Streaming handler terminated with exception: {}", e.what());
                }
                catch(...)
                {
                    spdlog::error("Streaming handler terminated with unknown exception");
                }
                co_return;
            }

            if (handler)
            {
                try
                {
                    response = co_await (*handler)(const_request, std::move(route_args), std::move(query_args));
                }
                catch(...)
                {
                    spdlog::error("Error while executing handler");
                    response.setVersion("HTTP/1.1");
                    response.setCode(http::Code::InternalServerError);
                    response.setHeader(http::Header::Connection, "close");
                    response.setBodyWithContentLength("500 Internal Server Error");

                }
            }
            else
            {
                response.setVersion("HTTP/1.1");
                response.setCode(http::Code::NotFound);
                response.setHeader(http::Header::Connection, "close");
                response.setBodyWithContentLength("404 Not Found");
            }

            // The status only: a response body can carry a freshly issued access token.
            if(spdlog::should_log(spdlog::level::debug))
            {
                spdlog::debug(std::format("Send response: {}", response.getCode()));
            }

            co_await writeData(sock, std::format("{}", response));

            // connection should close
            const auto connection_header = response.getHeader(http::Header::Connection);
            if(std::ranges::find(connection_header, "close") != connection_header.end())co_return;

            full_request_data.clear(); // prepare for next request on keep-alive
        }
    }

    asio::awaitable<void> Server::writeData(asio::ip::tcp::socket & sock, std::string message)
    {
        co_await asio::async_write(sock, asio::buffer(message), asio::use_awaitable);
    }
}
