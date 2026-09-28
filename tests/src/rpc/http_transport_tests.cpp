#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <memory>
#include <string>
#include <vector>

#include <asio/experimental/awaitable_operators.hpp>

#include "http_transport.hpp"
#include "json_rpc.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    // Answers each of `responses.size()` connections on 127.0.0.1 with the next canned
    // response. An empty response keeps its connection silent until the client leaves.
    class CannedHttpServer
    {
        public:
            CannedHttpServer(asio::io_context & io_context, std::vector<std::string> responses)
                : _acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0))
            {
                asio::co_spawn(io_context, _serve(std::move(responses)), asio::detached);
            }

            rpc::Endpoint endpoint() const
            {
                return rpc::Endpoint{
                    .tls = false,
                    .host = "127.0.0.1",
                    .port = std::to_string(_acceptor.local_endpoint().port()),
                    .target = "/rpc"};
            }

        private:
            asio::awaitable<void> _serve(std::vector<std::string> responses)
            {
                for(std::string & response : responses)
                {
                    std::error_code ec;
                    auto socket = co_await _acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));
                    if(ec) co_return;
                    asio::co_spawn(_acceptor.get_executor(), _answer(std::move(socket), std::move(response)), asio::detached);
                }
            }

            static asio::awaitable<void> _answer(asio::ip::tcp::socket socket, std::string response)
            {
                std::error_code ec;
                asio::streambuf request;
                co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::redirect_error(asio::use_awaitable, ec));
                if(ec) co_return;

                if(response.empty())
                {
                    char byte = 0;
                    (void)co_await socket.async_read_some(
                        asio::buffer(&byte, 1), asio::redirect_error(asio::use_awaitable, ec));
                    co_return;
                }

                co_await asio::async_write(socket, asio::buffer(response), asio::redirect_error(asio::use_awaitable, ec));
                socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            }

            asio::ip::tcp::acceptor _acceptor;
    };

    std::string httpResponse(const int status, const std::string & reason, const std::string & body)
    {
        return std::format(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            status, reason, body.size(), body);
    }

    std::string chunkedResponse(const std::vector<std::string> & chunks)
    {
        std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        for(const std::string & chunk : chunks)
        {
            response += std::format("{:x}\r\n{}\r\n", chunk.size(), chunk);
        }
        return response + "0\r\n\r\n";
    }

    const std::string RESULT_BODY = R"({"jsonrpc":"2.0","id":1,"result":"0x1"})";

    // Sends `count` requests one after another from a single coroutine: the canned server
    // keeps the io_context busy until it has accepted every connection it expects.
    template<class Send>
    auto sequentially(asio::io_context & io_context, const std::size_t count, Send send)
    {
        using Result = typename decltype(send())::value_type;
        return runAwaitable(io_context, [count, send]() -> asio::awaitable<std::vector<Result>>
        {
            std::vector<Result> results;
            for(std::size_t i = 0; i < count; ++i)
            {
                results.push_back(co_await send());
            }
            co_return results;
        }());
    }
}

TEST_F(UnitTest, Rpc_HttpClient_ReassemblesAChunkedBody)
{
    asio::io_context io_context;
    CannedHttpServer server(io_context, {chunkedResponse({RESULT_BODY.substr(0, 10), RESULT_BODY.substr(10)})});
    const auto client = rpc::makeHttpClient(io_context, server.endpoint());

    const auto response = runAwaitable(io_context, client->request("POST", "/rpc", "{}"));
    ASSERT_TRUE(response.has_value()) << response.error().message;
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, RESULT_BODY);
}

TEST_F(UnitTest, Rpc_HttpClient_RefusesAResponseAboveTheSizeLimit)
{
    asio::io_context io_context;
    CannedHttpServer server(io_context, {
        httpResponse(200, "OK", std::string(4096, 'a')),
        chunkedResponse({std::string(3000, 'a'), std::string(3000, 'b')})});
    const auto client = rpc::makeHttpClient(
        io_context, server.endpoint(), rpc::HttpTransportConfig{.max_response_bytes = 1024});

    const auto responses = sequentially(io_context, 2, [&client]() { return client->request("POST", "/rpc", "{}"); });
    ASSERT_EQ(responses.size(), 2u);
    for(const auto & response : responses)
    {
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(response.error().kind, rpc::RpcError::Kind::LIMIT);
    }
}

TEST_F(UnitTest, Rpc_HttpClient_TimesOutASilentServer)
{
    asio::io_context io_context;
    CannedHttpServer server(io_context, {""});
    const auto client = rpc::makeHttpClient(
        io_context, server.endpoint(), rpc::HttpTransportConfig{.request_timeout = std::chrono::milliseconds(200)});

    const auto started = std::chrono::steady_clock::now();
    const auto response = runAwaitable(io_context, client->request("POST", "/rpc", "{}"));
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().kind, rpc::RpcError::Kind::TIMEOUT);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
}

TEST_F(UnitTest, Rpc_HttpClient_ConcurrentRequestsDoNotShareAConnection)
{
    asio::io_context io_context;
    CannedHttpServer server(io_context, {
        httpResponse(200, "OK", R"({"jsonrpc":"2.0","id":1,"result":"0xa"})"),
        httpResponse(200, "OK", R"({"jsonrpc":"2.0","id":2,"result":"0xb"})")});
    const auto client = rpc::makeHttpClient(io_context, server.endpoint());

    const auto both = runAwaitable(io_context, [&client]() -> asio::awaitable<std::vector<std::string>>
    {
        using namespace asio::experimental::awaitable_operators;
        auto [first, second] = co_await (client->request("POST", "/rpc", "{}") && client->request("POST", "/rpc", "{}"));
        std::vector<std::string> bodies;
        if(first) bodies.push_back(first->body);
        if(second) bodies.push_back(second->body);
        co_return bodies;
    }());

    // Each request got one whole response of its own.
    ASSERT_EQ(both.size(), 2u);
    for(const std::string & body : both)
    {
        const auto parsed = json::parse(body, nullptr, false);
        ASSERT_FALSE(parsed.is_discarded()) << body;
        EXPECT_TRUE(parsed.contains("result"));
    }
    EXPECT_NE(both[0], both[1]);
}

TEST_F(UnitTest, Rpc_HttpTransport_MapsHttpStatusToTheJsonRpcErrorModel)
{
    asio::io_context io_context;
    CannedHttpServer server(io_context, {
        // A provider answering a JSON-RPC error with a 4xx status: the error object decides.
        httpResponse(400, "Bad Request", R"({"jsonrpc":"2.0","id":1,"error":{"code":3,"message":"execution reverted"}})"),
        httpResponse(429, "Too Many Requests", "{}"),
        httpResponse(503, "Service Unavailable", "{}"),
        httpResponse(404, "Not Found", "<html>not found</html>")});

    const auto transport = rpc::makeHttpTransport(
        io_context, std::format("http://127.0.0.1:{}/rpc", server.endpoint().port));
    ASSERT_NE(transport, nullptr);

    const auto responses = sequentially(io_context, 4, [&transport]() { return transport->post("{}"); });
    ASSERT_EQ(responses.size(), 4u);

    const auto & error_body = responses[0];
    ASSERT_TRUE(error_body.has_value());
    EXPECT_NE(error_body->find("execution reverted"), std::string::npos);

    ASSERT_FALSE(responses[1].has_value());
    EXPECT_EQ(responses[1].error().kind, rpc::RpcError::Kind::THROTTLED);

    ASSERT_FALSE(responses[2].has_value());
    EXPECT_EQ(responses[2].error().kind, rpc::RpcError::Kind::TRANSPORT);

    ASSERT_FALSE(responses[3].has_value());
    EXPECT_EQ(responses[3].error().kind, rpc::RpcError::Kind::PROTOCOL);

    // The description never carries the URL target, which commonly holds an API key.
    EXPECT_EQ(std::string(transport->describe()).find("/rpc"), std::string::npos);
}
