#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "eth_client.hpp"
#include "http_transport.hpp"
#include "json_rpc.hpp"
#include "rpc_log_source.hpp"
#include "registry_events.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    std::string okBody(const json & request, json result)
    {
        return json{{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}}.dump();
    }

    std::string errorBody(const json & request, const int code, const std::string & message)
    {
        return json{
            {"jsonrpc", "2.0"},
            {"id", request.at("id")},
            {"error", json{{"code", code}, {"message", message}}}
        }.dump();
    }

    // Answers JSON-RPC requests from a test-supplied responder without touching a
    // socket, and records what the client actually asked for.
    class FakeTransport final : public rpc::ITransport
    {
        public:
            using Responder = std::function<std::string(const json &)>;

            explicit FakeTransport(Responder responder)
                : _responder(std::move(responder))
            {
            }

            asio::awaitable<std::expected<std::string, rpc::RpcError>> post(std::string body) override
            {
                const json request = json::parse(body, nullptr, false);
                requests.push_back(request);

                if(!transport_failures.empty())
                {
                    const rpc::RpcError error = transport_failures.front();
                    transport_failures.pop_front();
                    co_return std::unexpected(error);
                }

                co_return _responder(request);
            }

            std::string_view describe() const override { return "fake://provider"; }

            std::vector<json> requests;
            std::deque<rpc::RpcError> transport_failures;

        private:
            Responder _responder;
    };

    std::shared_ptr<rpc::EthClient> makeClient(std::shared_ptr<FakeTransport> transport)
    {
        return std::make_shared<rpc::EthClient>(
            std::make_shared<rpc::JsonRpcClient>(
                std::move(transport), rpc::JsonRpcConfig{.max_attempts = 3, .initial_backoff_ms = 0, .max_backoff_ms = 0}));
    }

    const std::string TRANSFORMATION_ADDED_SIG =
        "TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)";

    std::string blockHashFor(const std::int64_t block_number, const std::uint8_t variant = 0x10)
    {
        return hexBytes(static_cast<std::uint8_t>((block_number + variant) & 0xFF), 32);
    }

    json makeRpcLogJson(const std::int64_t block_number, const std::int64_t log_index, const std::string & block_hash)
    {
        return json{
            {"blockNumber", chain::toHexQuantity(block_number)},
            {"blockHash", block_hash},
            {"transactionHash", hexBytes(0x22, 32)},
            {"transactionIndex", "0x0"},
            {"logIndex", chain::toHexQuantity(log_index)},
            {"address", hexAddress(0xAB)},
            {"data", encodeSimpleAddedEventDataV2(
                makeAddressFromByte(0x41), "rpc_tx", makeAddressFromByte(0x44), makeAddressFromByte(0x42), 1)},
            {"topics", json::array({topicForEvent(TRANSFORMATION_ADDED_SIG)})},
            {"removed", false}
        };
    }

    json makeRpcBlockJson(const std::int64_t block_number, const std::string & block_hash)
    {
        return json{
            {"number", chain::toHexQuantity(block_number)},
            {"hash", block_hash},
            {"parentHash", blockHashFor(block_number - 1)},
            {"timestamp", chain::toHexQuantity(1'700'000'000 + block_number)}
        };
    }

    events::RpcLogSourceConfig makeSourceConfig()
    {
        return events::RpcLogSourceConfig{
            .source_id = "eip155:1:0x00000000000000000000000000000000000000ab",
            .chain_namespace = "eip155:1",
            .chain_id = CHAIN_ID,
            .registry_address = hexAddress(0xAB),
            .deployment_block = 1,
            .max_batch_blocks = 100,
            .confirmations = 2
        };
    }

    // Serves a chain whose head/safe/finalized heights and per-range logs are supplied
    // by the test.
    struct FakeChain
    {
        std::int64_t head = 0;
        std::int64_t safe = 0;
        std::int64_t finalized = 0;
        std::vector<json> logs;
        int forced_limit_errors = 0;
        std::vector<std::pair<std::int64_t, std::int64_t>> log_queries;
        std::function<std::string(const json &)> call_responder;

        std::string respond(const json & request)
        {
            const std::string method = request.at("method").get<std::string>();
            const json & params = request.at("params");

            if(method == "eth_blockNumber")
            {
                return okBody(request, chain::toHexQuantity(head));
            }

            if(method == "eth_getBlockByNumber")
            {
                const std::string tag = params.at(0).get<std::string>();
                if(tag == "latest") return okBody(request, makeRpcBlockJson(head, blockHashFor(head)));
                if(tag == "safe") return okBody(request, makeRpcBlockJson(safe, blockHashFor(safe)));
                if(tag == "finalized") return okBody(request, makeRpcBlockJson(finalized, blockHashFor(finalized)));

                const auto number = parse::parseHexQuantity(tag);
                if(!number) return errorBody(request, -32602, "bad block tag");
                return okBody(request, makeRpcBlockJson(*number, blockHashFor(*number)));
            }

            if(method == "eth_call")
            {
                return call_responder ? call_responder(request) : errorBody(request, 3, "execution reverted");
            }

            if(method == "eth_getLogs")
            {
                const json & filter = params.at(0);
                const auto from = parse::parseHexQuantity(filter.at("fromBlock").get<std::string>());
                const auto to = parse::parseHexQuantity(filter.at("toBlock").get<std::string>());
                log_queries.emplace_back(*from, *to);

                if(forced_limit_errors > 0)
                {
                    --forced_limit_errors;
                    return errorBody(request, -32005, "query returned more than 10000 results");
                }

                json matched = json::array();
                for(const json & log : logs)
                {
                    const auto block = parse::parseHexQuantity(log.at("blockNumber").get<std::string>());
                    if(block && *block >= *from && *block <= *to)
                    {
                        matched.push_back(log);
                    }
                }
                return okBody(request, std::move(matched));
            }

            return errorBody(request, -32601, "method not found");
        }
    };
}

// ---- endpoint parsing and redaction ----

TEST_F(UnitTest, Rpc_ParseEndpoint_AcceptsProviderUrlAndRedactsIt)
{
    const auto endpoint = rpc::parseEndpoint("https://eth-sepolia.example.com/v2/SECRET_API_KEY");
    ASSERT_TRUE(endpoint.has_value());
    EXPECT_TRUE(endpoint->tls);
    EXPECT_EQ(endpoint->host, "eth-sepolia.example.com");
    EXPECT_EQ(endpoint->port, "443");
    EXPECT_EQ(endpoint->target, "/v2/SECRET_API_KEY");
    EXPECT_EQ(endpoint->redacted().find("SECRET_API_KEY"), std::string::npos);
}

TEST_F(UnitTest, Rpc_ParseEndpoint_RejectsUnsupportedSchemeAndEmbeddedCredentials)
{
    EXPECT_FALSE(rpc::parseEndpoint("ws://node.example.com").has_value());
    EXPECT_FALSE(rpc::parseEndpoint("node.example.com").has_value());
    EXPECT_FALSE(rpc::parseEndpoint("https://user:pass@node.example.com/").has_value());
    EXPECT_FALSE(rpc::parseEndpoint("http://node.example.com:notaport/").has_value());
}

TEST_F(UnitTest, Rpc_ParseEndpoint_UsesExplicitPortAndDefaultTarget)
{
    const auto endpoint = rpc::parseEndpoint("http://127.0.0.1:8545");
    ASSERT_TRUE(endpoint.has_value());
    EXPECT_FALSE(endpoint->tls);
    EXPECT_EQ(endpoint->port, "8545");
    EXPECT_EQ(endpoint->target, "/");
}

// ---- JSON-RPC client semantics ----

TEST_F(UnitTest, Rpc_JsonRpcClient_RetriesTransportFailuresThenSucceeds)
{
    asio::io_context io_context;

    auto transport = std::make_shared<FakeTransport>(
        [](const json & request) { return okBody(request, "0x1"); });
    transport->transport_failures.push_back(
        rpc::RpcError{.kind = rpc::RpcError::Kind::TRANSPORT, .code = 0, .message = "socket closed"});

    rpc::JsonRpcClient client(
        transport, rpc::JsonRpcConfig{.max_attempts = 3, .initial_backoff_ms = 0, .max_backoff_ms = 0});

    const auto result = runAwaitable(io_context, client.call("eth_chainId", json::array()));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get<std::string>(), "0x1");
    EXPECT_EQ(transport->requests.size(), 2u);
}

TEST_F(UnitTest, Rpc_JsonRpcClient_DoesNotRetryDeterministicProviderErrors)
{
    asio::io_context io_context;

    auto transport = std::make_shared<FakeTransport>(
        [](const json & request) { return errorBody(request, 3, "execution reverted"); });

    rpc::JsonRpcClient client(
        transport, rpc::JsonRpcConfig{.max_attempts = 3, .initial_backoff_ms = 0, .max_backoff_ms = 0});

    const auto result = runAwaitable(io_context, client.call("eth_call", json::array()));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, rpc::RpcError::Kind::EXECUTION);
    EXPECT_EQ(transport->requests.size(), 1u);
}

TEST_F(UnitTest, Rpc_JsonRpcClient_ClassifiesRangeLimitsSeparatelyFromThrottling)
{
    asio::io_context io_context;

    auto limit_transport = std::make_shared<FakeTransport>(
        [](const json & request) { return errorBody(request, -32005, "query returned more than 10000 results"); });
    rpc::JsonRpcClient limit_client(
        limit_transport, rpc::JsonRpcConfig{.max_attempts = 3, .initial_backoff_ms = 0, .max_backoff_ms = 0});

    const auto limited = runAwaitable(io_context, limit_client.call("eth_getLogs", json::array()));
    ASSERT_FALSE(limited.has_value());
    EXPECT_EQ(limited.error().kind, rpc::RpcError::Kind::LIMIT);
    // A too-wide range must be narrowed by the caller, never blindly re-sent.
    EXPECT_EQ(limit_transport->requests.size(), 1u);

    auto throttle_transport = std::make_shared<FakeTransport>(
        [](const json & request) { return errorBody(request, -32005, "your app has exceeded its compute units"); });
    rpc::JsonRpcClient throttle_client(
        throttle_transport, rpc::JsonRpcConfig{.max_attempts = 3, .initial_backoff_ms = 0, .max_backoff_ms = 0});

    const auto throttled = runAwaitable(io_context, throttle_client.call("eth_getLogs", json::array()));
    ASSERT_FALSE(throttled.has_value());
    EXPECT_EQ(throttled.error().kind, rpc::RpcError::Kind::THROTTLED);
    EXPECT_EQ(throttle_transport->requests.size(), 3u);
}

TEST_F(UnitTest, Rpc_JsonRpcClient_DoesNotRetryAnExhaustedQuota)
{
    asio::io_context io_context;

    for(const std::string message : {
        std::string("Monthly capacity limit exceeded. Visit the dashboard to upgrade your scaling policy"),
        std::string("daily request count exceeded, request rate limited")})
    {
        auto transport = std::make_shared<FakeTransport>(
            [&message](const json & request) { return errorBody(request, 429, message); });
        rpc::JsonRpcClient client(
            transport, rpc::JsonRpcConfig{.max_attempts = 3, .initial_backoff_ms = 0, .max_backoff_ms = 0});

        const auto result = runAwaitable(io_context, client.call("eth_chainId", json::array()));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, rpc::RpcError::Kind::QUOTA) << message;
        // Retrying cannot succeed before the provider's period resets.
        EXPECT_EQ(transport->requests.size(), 1u) << message;
    }
}

TEST_F(UnitTest, Rpc_JsonRpcClient_RejectsMismatchedResponseId)
{
    asio::io_context io_context;

    auto transport = std::make_shared<FakeTransport>([](const json &)
    {
        return json{{"jsonrpc", "2.0"}, {"id", 9999}, {"result", "0x1"}}.dump();
    });

    rpc::JsonRpcClient client(
        transport, rpc::JsonRpcConfig{.max_attempts = 2, .initial_backoff_ms = 0, .max_backoff_ms = 0});

    const auto result = runAwaitable(io_context, client.call("eth_chainId", json::array()));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, rpc::RpcError::Kind::PROTOCOL);
}

TEST_F(UnitTest, Rpc_JsonRpcClient_RejectsMalformedResponseBody)
{
    asio::io_context io_context;

    auto transport = std::make_shared<FakeTransport>([](const json &) { return std::string("<html>502</html>"); });
    rpc::JsonRpcClient client(
        transport, rpc::JsonRpcConfig{.max_attempts = 1, .initial_backoff_ms = 0, .max_backoff_ms = 0});

    const auto result = runAwaitable(io_context, client.call("eth_chainId", json::array()));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, rpc::RpcError::Kind::PROTOCOL);
}

// ---- strict quantity / data parsing ----

TEST_F(UnitTest, Rpc_EthClient_RejectsNonHexQuantities)
{
    asio::io_context io_context;

    auto decimal_transport = std::make_shared<FakeTransport>(
        [](const json & request) { return okBody(request, "11155111"); });
    const auto decimal = runAwaitable(io_context, makeClient(decimal_transport)->chainId());
    ASSERT_FALSE(decimal.has_value());
    EXPECT_EQ(decimal.error().kind, rpc::RpcError::Kind::PROTOCOL);

    auto numeric_transport = std::make_shared<FakeTransport>(
        [](const json & request) { return okBody(request, 11155111); });
    const auto numeric = runAwaitable(io_context, makeClient(numeric_transport)->blockNumber());
    ASSERT_FALSE(numeric.has_value());
    EXPECT_EQ(numeric.error().kind, rpc::RpcError::Kind::PROTOCOL);

    auto valid_transport = std::make_shared<FakeTransport>(
        [](const json & request) { return okBody(request, "0xaa36a7"); });
    const auto valid = runAwaitable(io_context, makeClient(valid_transport)->chainId());
    ASSERT_TRUE(valid.has_value());
    EXPECT_EQ(*valid, 11155111u);
}

TEST_F(UnitTest, Rpc_EthClient_RejectsMalformedAndOversizedData)
{
    asio::io_context io_context;

    auto odd_transport = std::make_shared<FakeTransport>(
        [](const json & request) { return okBody(request, "0xabc"); });
    const auto odd = runAwaitable(io_context, makeClient(odd_transport)->getCode(hexAddress(0xAB)));
    ASSERT_FALSE(odd.has_value());
    EXPECT_EQ(odd.error().kind, rpc::RpcError::Kind::PROTOCOL);

    auto huge_transport = std::make_shared<FakeTransport>([](const json & request)
    {
        return okBody(request, std::string("0x") + std::string(9ull * 1024ull * 1024ull, 'a'));
    });
    const auto huge = runAwaitable(io_context, makeClient(huge_transport)->getCode(hexAddress(0xAB)));
    ASSERT_FALSE(huge.has_value());
    EXPECT_EQ(huge.error().kind, rpc::RpcError::Kind::LIMIT);
}

TEST_F(UnitTest, Rpc_VerifyPtRegistry_RejectsChainIdMismatchAndEmptyCode)
{
    asio::io_context io_context;

    auto wrong_chain = std::make_shared<FakeTransport>([](const json & request)
    {
        return okBody(request, "0x1");
    });
    const auto mismatch = runAwaitable(
        io_context, rpc::verifyPtRegistry(*makeClient(wrong_chain), 11155111, hexAddress(0xAB)));
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_NE(mismatch.error().find("chain id"), std::string::npos);

    auto no_code = std::make_shared<FakeTransport>([](const json & request)
    {
        const std::string method = request.at("method").get<std::string>();
        if(method == "eth_chainId") return okBody(request, "0x1");
        if(method == "eth_getCode") return okBody(request, "0x");
        return errorBody(request, -32601, "method not found");
    });
    const auto empty = runAwaitable(
        io_context, rpc::verifyPtRegistry(*makeClient(no_code), 1, hexAddress(0xAB)));
    ASSERT_FALSE(empty.has_value());
    EXPECT_NE(empty.error().find("no deployed code"), std::string::npos);
}

// ---- RpcLogSource cursor behaviour ----

TEST_F(UnitTest, Rpc_LogSource_EmptyRangeAdvancesCursorAndReportsScannedRange)
{
    asio::io_context io_context;

    FakeChain chain{.head = 40, .safe = 30, .finalized = 20};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(21, 256));

    EXPECT_TRUE(poll.records.empty());
    EXPECT_EQ(poll.next_cursor, 41u);
    ASSERT_TRUE(poll.scanned_range.has_value());
    EXPECT_EQ(poll.scanned_range->from_block, 1);
    EXPECT_EQ(poll.scanned_range->to_block, 40);
    ASSERT_TRUE(poll.finality.has_value());
    EXPECT_EQ(poll.finality->finalized, 20);
    EXPECT_EQ(poll.finality->safe, 30);
    EXPECT_EQ(poll.finality->finalized_hash, blockHashFor(20));
    EXPECT_TRUE(poll.caught_up);
    const auto metrics = source.metrics();
    EXPECT_EQ(metrics.polls, 1u);
    EXPECT_EQ(metrics.range_requests, 1u);
    EXPECT_EQ(metrics.scanned_blocks, 40u);
    EXPECT_EQ(metrics.logs_received, 0u);
    EXPECT_EQ(metrics.lag_blocks, 0);
}

TEST_F(UnitTest, Rpc_LogSource_FailedRangeLeavesCursorAndFinalityUnchanged)
{
    asio::io_context io_context;

    FakeChain chain{.head = 40, .safe = 30, .finalized = 20};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & request)
    {
        if(request.at("method").get<std::string>() == "eth_getLogs")
        {
            return errorBody(request, -32602, "invalid filter");
        }
        return chain.respond(request);
    });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(21, 256));

    EXPECT_EQ(poll.next_cursor, 21u);
    EXPECT_FALSE(poll.scanned_range.has_value());
    // A failed poll proves nothing, so it must not promote any row.
    EXPECT_FALSE(poll.finality.has_value());
    EXPECT_EQ(source.metrics().failed_polls, 1u);
}

TEST_F(UnitTest, Rpc_LogSource_SplitsOversizedRangeDownToOneBlock)
{
    asio::io_context io_context;

    FakeChain chain{.head = 100, .safe = 90, .finalized = 80};
    // Widths 20, 10, 5 and 2 are refused, so the source must keep halving until the
    // single-block query is accepted rather than stalling on the original range.
    chain.forced_limit_errors = 4;
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSourceConfig config = makeSourceConfig();
    config.deployment_block = 81;
    events::RpcLogSource source(makeClient(transport), config);

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(81, 256));

    ASSERT_FALSE(chain.log_queries.empty());
    const auto & last = chain.log_queries.back();
    EXPECT_EQ(last.first, 81);
    EXPECT_EQ(last.second, 81);
    ASSERT_TRUE(poll.scanned_range.has_value());
    EXPECT_EQ(poll.scanned_range->from_block, 81);
    EXPECT_EQ(poll.scanned_range->to_block, 81);
    EXPECT_EQ(poll.next_cursor, 82u);
    EXPECT_FALSE(poll.caught_up);
    const auto metrics = source.metrics();
    EXPECT_EQ(metrics.range_requests, 5u);
    EXPECT_EQ(metrics.provider_limit_responses, 4u);
    EXPECT_EQ(metrics.scanned_blocks, 1u);
}

TEST_F(UnitTest, Rpc_LogSource_BackfillSpanningSeveralBatchesAdvancesFinalityPerBatch)
{
    asio::io_context io_context;

    FakeChain chain{.head = 250, .safe = 245, .finalized = 240};
    chain.logs.push_back(makeRpcLogJson(150, 0, blockHashFor(150)));
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());

    const events::SourcePoll first = runAwaitable(io_context, source.pollSince(0, 256));
    source.committed();
    const events::SourcePoll second = runAwaitable(io_context, source.pollSince(first.next_cursor, 256));
    source.committed();
    const events::SourcePoll third = runAwaitable(io_context, source.pollSince(second.next_cursor, 256));
    source.committed();

    ASSERT_TRUE(first.scanned_range && second.scanned_range && third.scanned_range);
    EXPECT_EQ(first.scanned_range->from_block, 1);
    EXPECT_EQ(first.scanned_range->to_block, 100);
    EXPECT_EQ(second.scanned_range->from_block, 101);
    EXPECT_EQ(second.scanned_range->to_block, 200);
    EXPECT_EQ(third.scanned_range->from_block, 201);
    EXPECT_EQ(third.scanned_range->to_block, 250);

    // Backlog keeps ingestion polling without waiting out the idle interval.
    EXPECT_FALSE(first.caught_up);
    EXPECT_FALSE(second.caught_up);
    EXPECT_TRUE(third.caught_up);

    ASSERT_TRUE(first.finality && second.finality && third.finality);
    EXPECT_EQ(first.finality->finalized, 100);
    EXPECT_EQ(second.finality->finalized, 200);
    EXPECT_EQ(third.finality->finalized, 240);

    EXPECT_TRUE(first.records.empty());
    ASSERT_EQ(second.records.size(), 1u);
    EXPECT_EQ(second.records.front().block_number, 150);
    EXPECT_EQ(third.next_cursor, 251u);
}

TEST_F(UnitTest, Rpc_LogSource_UncommittedPollIsScannedAgain)
{
    asio::io_context io_context;

    FakeChain chain{.head = 50, .safe = 49, .finalized = 48};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());

    const events::SourcePoll first = runAwaitable(io_context, source.pollSince(1, 256));
    ASSERT_TRUE(first.scanned_range.has_value());
    ASSERT_TRUE(first.finality.has_value());

    // The runtime failed to commit the first batch, so its range must be delivered again.
    const events::SourcePoll retried = runAwaitable(io_context, source.pollSince(1, 256));
    ASSERT_TRUE(retried.scanned_range.has_value());
    ASSERT_TRUE(retried.finality.has_value());
    EXPECT_EQ(retried.scanned_range->from_block, first.scanned_range->from_block);
    EXPECT_EQ(retried.finality->finalized, first.finality->finalized);
}

TEST_F(UnitTest, Rpc_LogSource_RestartRescansFromAppliedFinality)
{
    asio::io_context io_context;

    FakeChain chain{.head = 50, .safe = 46, .finalized = 44};
    chain.logs.push_back(makeRpcLogJson(48, 0, blockHashFor(48)));
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());
    source.restoreFinality(events::FinalityCheckpoint{.finalized_block = 44, .finalized_hash = blockHashFor(44)});

    // The cursor says 50, but blocks 45..49 were never final: they are scanned again
    // rather than trusted.
    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(50, 256));

    ASSERT_TRUE(poll.scanned_range.has_value());
    EXPECT_EQ(poll.scanned_range->from_block, 45);
    EXPECT_EQ(poll.scanned_range->to_block, 50);
    EXPECT_EQ(poll.next_cursor, 51u);
    ASSERT_EQ(poll.records.size(), 1u);
    EXPECT_EQ(poll.records.front().block_number, 48);
    EXPECT_EQ(poll.records.front().block_time, 1'700'000'000 + 48);
}

TEST_F(UnitTest, Rpc_LogSource_NeverRescansBelowAppliedFinality)
{
    asio::io_context io_context;

    FakeChain chain{.head = 50, .safe = 49, .finalized = 48};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());

    (void)runAwaitable(io_context, source.pollSince(45, 256));
    source.committed();

    chain.head = 60;
    chain.safe = 59;
    chain.finalized = 58;
    const events::SourcePoll second = runAwaitable(io_context, source.pollSince(51, 256));

    ASSERT_TRUE(second.scanned_range.has_value());
    EXPECT_EQ(second.scanned_range->from_block, 49);
}

TEST_F(UnitTest, Rpc_LogSource_NeverFinalizesPastWhatTheChainFinalized)
{
    asio::io_context io_context;

    // A registry deployed moments ago: the chain has not finalized even the block before it.
    FakeChain chain{.head = 50, .safe = 25, .finalized = 20};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSourceConfig config = makeSourceConfig();
    config.deployment_block = 30;
    events::RpcLogSource source(makeClient(transport), config);

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(30, 256));

    ASSERT_TRUE(poll.scanned_range.has_value());
    EXPECT_EQ(poll.scanned_range->from_block, 30);
    ASSERT_TRUE(poll.finality.has_value());
    // The anchor a later poll checks for deep reorgs is a block the chain itself finalized.
    EXPECT_EQ(poll.finality->finalized, 20);
    EXPECT_EQ(poll.finality->finalized_hash, blockHashFor(20));
}

TEST_F(UnitTest, Rpc_LogSource_PausesWhenAFinalizedBlockIsReplaced)
{
    asio::io_context io_context;

    FakeChain chain{.head = 50, .safe = 49, .finalized = 48};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());
    // The server finalized block 44 with a hash the provider no longer reports.
    source.restoreFinality(events::FinalityCheckpoint{.finalized_block = 44, .finalized_hash = hexBytes(0xEE, 32)});

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(45, 256));

    EXPECT_TRUE(source.paused());
    EXPECT_FALSE(poll.scanned_range.has_value());
    EXPECT_FALSE(poll.finality.has_value());
    EXPECT_EQ(poll.next_cursor, 45u);

    // A paused source stays paused: it never ingests from the replaced history.
    const events::SourcePoll later = runAwaitable(io_context, source.pollSince(45, 256));
    EXPECT_FALSE(later.scanned_range.has_value());
}

TEST_F(UnitTest, Rpc_LogSource_PausesWhenTheFinalizedTagReplacesTheAnchor)
{
    asio::io_context io_context;

    FakeChain chain{.head = 50, .safe = 49, .finalized = 48};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());
    source.restoreFinality(events::FinalityCheckpoint{.finalized_block = 48, .finalized_hash = hexBytes(0xEE, 32)});

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(49, 256));

    EXPECT_TRUE(source.paused());
    EXPECT_FALSE(poll.scanned_range.has_value());
    // The finalized tag named block 48, so its hash was checked without reading it again.
    EXPECT_EQ(transport->requests.size(), 3u);
}

TEST_F(UnitTest, Rpc_LogSource_CaughtUpPollReadsOnlyTheHeadAndTheLogs)
{
    asio::io_context io_context;

    const auto methodsOf = [](const std::vector<json> & requests, const std::size_t from)
    {
        std::vector<std::string> methods;
        for(std::size_t i = from; i < requests.size(); ++i)
        {
            const json & request = requests[i];
            std::string method = request.at("method").get<std::string>();
            if(method == "eth_getBlockByNumber") method += ":" + request.at("params").at(0).get<std::string>();
            methods.push_back(std::move(method));
        }
        return methods;
    };

    FakeChain chain{.head = 50, .safe = 49, .finalized = 48};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());

    const events::SourcePoll first = runAwaitable(io_context, source.pollSince(1, 256));
    source.committed();
    ASSERT_TRUE(first.finality.has_value());
    EXPECT_EQ(first.finality->finalized, 48);
    EXPECT_EQ(methodsOf(transport->requests, 0), (std::vector<std::string>{
        "eth_getBlockByNumber:latest",
        "eth_getBlockByNumber:safe",
        "eth_getBlockByNumber:finalized",
        "eth_getLogs"}));

    // Within the refresh interval the finality tags are reused, the anchor is checked
    // against the cached finalized header, and the head header ends the range.
    chain.head = 51;
    const std::size_t before_second = transport->requests.size();
    const events::SourcePoll second = runAwaitable(io_context, source.pollSince(first.next_cursor, 256));
    source.committed();
    ASSERT_TRUE(second.scanned_range.has_value());
    EXPECT_EQ(second.scanned_range->from_block, 49);
    EXPECT_EQ(second.scanned_range->to_block, 51);
    ASSERT_TRUE(second.finality.has_value());
    EXPECT_EQ(second.finality->finalized, 48);
    EXPECT_EQ(methodsOf(transport->requests, before_second), (std::vector<std::string>{
        "eth_getBlockByNumber:latest",
        "eth_getLogs"}));

    // Once the interval has passed the tags are read again.
    events::RpcLogSourceConfig config = makeSourceConfig();
    config.finality_refresh_interval = std::chrono::milliseconds(0);
    events::RpcLogSource refreshing(makeClient(transport), config);
    (void)runAwaitable(io_context, refreshing.pollSince(1, 256));
    refreshing.committed();
    chain.finalized = 50;
    const std::size_t before_refresh = transport->requests.size();
    const events::SourcePoll refreshed = runAwaitable(io_context, refreshing.pollSince(52, 256));
    ASSERT_TRUE(refreshed.finality.has_value());
    EXPECT_EQ(refreshed.finality->finalized, 50);
    EXPECT_EQ(methodsOf(transport->requests, before_refresh), (std::vector<std::string>{
        "eth_getBlockByNumber:latest",
        "eth_getBlockByNumber:safe",
        "eth_getBlockByNumber:finalized",
        "eth_getBlockByNumber:0x30",
        "eth_getLogs"}));
}

TEST_F(UnitTest, Rpc_LogSource_DropsLogsTheFilterDidNotAskFor)
{
    asio::io_context io_context;

    FakeChain chain{.head = 20, .safe = 20, .finalized = 20};
    json foreign = makeRpcLogJson(10, 0, blockHashFor(10));
    foreign["address"] = hexAddress(0xCD);
    chain.logs.push_back(std::move(foreign));
    chain.logs.push_back(makeRpcLogJson(11, 0, blockHashFor(11)));
    auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(1, 256));

    ASSERT_EQ(poll.records.size(), 1u);
    EXPECT_EQ(poll.records.front().block_number, 11);
    EXPECT_EQ(source.metrics().unparsable_logs, 1u);
}

TEST_F(UnitTest, Rpc_LogSource_ProxyUpgradeIsCheckedAndNeverStored)
{
    asio::io_context io_context;

    const auto makeUpgradedLog = []()
    {
        json log = makeRpcLogJson(12, 1, blockHashFor(12));
        log["topics"] = json::array({pt::eventTopic(pt::PROXY_UPGRADED_SIGNATURE), hexBytes(0x00, 32)});
        log["data"] = "0x";
        return log;
    };

    // An implementation that still reports the supported protocol is accepted.
    {
        FakeChain chain{.head = 20, .safe = 20, .finalized = 20};
        chain.logs.push_back(makeUpgradedLog());
        chain.call_responder = [](const json & request)
        {
            const std::string data = request.at("params").at(0).value("data", "");
            const auto protocol_id = chain::constructSelector("protocolId()");
            if(data.starts_with(toHexPrefixed(std::span<const std::uint8_t>(protocol_id.data(), protocol_id.size()))))
            {
                return okBody(request, rpc::ptProtocolId());
            }
            return okBody(request, toHexPrefixed(std::span<const std::uint8_t>(encodeUint256Word(rpc::PT_PROTOCOL_VERSION))));
        };
        auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });
        events::RpcLogSource source(makeClient(transport), makeSourceConfig());

        const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(1, 256));
        EXPECT_FALSE(source.paused());
        ASSERT_TRUE(poll.scanned_range.has_value());
        EXPECT_TRUE(poll.records.empty());
    }

    // One whose identity cannot be confirmed pauses ingestion before anything from it lands.
    {
        FakeChain chain{.head = 20, .safe = 20, .finalized = 20};
        chain.logs.push_back(makeUpgradedLog());
        chain.logs.push_back(makeRpcLogJson(13, 0, blockHashFor(13)));
        auto transport = std::make_shared<FakeTransport>([&chain](const json & r) { return chain.respond(r); });
        events::RpcLogSource source(makeClient(transport), makeSourceConfig());

        const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(1, 256));
        EXPECT_TRUE(source.paused());
        EXPECT_FALSE(poll.scanned_range.has_value());
        EXPECT_TRUE(poll.records.empty());
    }
}

TEST_F(UnitTest, Rpc_LogSource_FallsBackToConfirmationDepthWithoutFinalityTags)
{
    asio::io_context io_context;

    FakeChain chain{.head = 30, .safe = 30, .finalized = 30};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & request)
    {
        if(request.at("method").get<std::string>() == "eth_getBlockByNumber")
        {
            const std::string tag = request.at("params").at(0).get<std::string>();
            if(tag == "safe" || tag == "finalized")
            {
                return okBody(request, json(nullptr));
            }
        }
        return chain.respond(request);
    });

    events::RpcLogSourceConfig config = makeSourceConfig();
    config.confirmations = 12;
    events::RpcLogSource source(makeClient(transport), config);

    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(1, 256));

    ASSERT_TRUE(poll.finality.has_value());
    EXPECT_EQ(poll.finality->head, 30);
    EXPECT_EQ(poll.finality->finalized, 18);
}

TEST_F(UnitTest, Rpc_LogSource_UnreachableFinalityTagSkipsThePoll)
{
    asio::io_context io_context;

    FakeChain chain{.head = 30, .safe = 25, .finalized = 20};
    auto transport = std::make_shared<FakeTransport>([&chain](const json & request)
    {
        // A server-side fault says nothing about whether the tag is supported.
        if(request.at("method").get<std::string>() == "eth_getBlockByNumber"
           && request.at("params").at(0).get<std::string>() == "finalized")
        {
            return errorBody(request, -32603, "internal error");
        }
        return chain.respond(request);
    });

    events::RpcLogSource source(makeClient(transport), makeSourceConfig());
    const events::SourcePoll poll = runAwaitable(io_context, source.pollSince(1, 256));

    // Neither a scan nor a fallback to confirmation depth: the poll proves nothing.
    EXPECT_FALSE(poll.scanned_range.has_value());
    EXPECT_FALSE(poll.finality.has_value());
    EXPECT_EQ(source.metrics().failed_polls, 1u);
}

// ---- scanned-range reconciliation in the hot store ----

namespace
{
    constexpr const char * RPC_SOURCE = "eip155:1:0xregistry";

    events::DecodedEvent makeRemoteEvent(const std::int64_t block_number, const std::uint8_t block_hash_byte)
    {
        return makeDecodedEvent(
            block_number, 0, 0, block_hash_byte, 0x22, events::EventType::TRANSFORMATION_ADDED);
    }
}

TEST_F(UnitTest, Rpc_HotStore_ScannedRangeRemovesLogWhoseReplacementRangeIsEmpty)
{
    asio::io_context io_context;
    const auto paths = makeTempEventsPaths("rpc_scan_removes_missing_log");

    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent event = makeRemoteEvent(10, 0x10);
    const events::ChainBlockInfo block =
        makeBlockInfo(10, event.raw.block_hash, hexBytes(0x7F, 32), 1'700'000'000, 1'000);

    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {event.raw}, {event}, {block}, 11, 1'000,
        events::ScannedBlockRange{.from_block = 10, .to_block = 10}));
    ASSERT_TRUE(store.applyFinality(
        RPC_SOURCE, CHAIN_ID, events::FinalityHeights{.head = 10, .safe = 0, .finalized = 0}, 1'000, 2048));

    {
        SqliteReadonly db(paths.hot_db);
        EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot;"), "observed");
    }

    // The replacement block carries no PT event at all: only a complete rescan of the
    // range can tell that the original log is gone.
    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {}, {}, {}, 11, 2'000,
        events::ScannedBlockRange{.from_block = 10, .to_block = 10}));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot;"), "removed");
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot;"), "removed");
    EXPECT_EQ(db.scalarInt64("SELECT removed FROM raw_events_hot;"), 1);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM reorg_window;"), 0);
}

TEST_F(UnitTest, Rpc_HotStore_ScannedRangeNeverRollsBackFinalizedRows)
{
    asio::io_context io_context;
    const auto paths = makeTempEventsPaths("rpc_scan_keeps_finalized");

    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent event = makeRemoteEvent(10, 0x10);
    const events::ChainBlockInfo block =
        makeBlockInfo(10, event.raw.block_hash, hexBytes(0x7F, 32), 1'700'000'000, 1'000);

    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {event.raw}, {event}, {block}, 11, 1'000,
        events::ScannedBlockRange{.from_block = 10, .to_block = 10}));
    ASSERT_TRUE(store.applyFinality(
        RPC_SOURCE, CHAIN_ID, events::FinalityHeights{.head = 20, .safe = 20, .finalized = 20}, 1'000, 2048));

    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {}, {}, {}, 11, 2'000,
        events::ScannedBlockRange{.from_block = 10, .to_block = 10}));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot;"), "finalized");
    EXPECT_EQ(db.scalarText("SELECT state FROM raw_events_hot;"), "finalized");
}

TEST_F(UnitTest, Rpc_HotStore_ScannedRangeKeepsLogsOutsideTheRange)
{
    asio::io_context io_context;
    const auto paths = makeTempEventsPaths("rpc_scan_bounded_by_range");

    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent kept = makeRemoteEvent(5, 0x05);
    const events::DecodedEvent removed = makeRemoteEvent(10, 0x10);

    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {kept.raw, removed.raw}, {kept, removed},
        {makeBlockInfo(5, kept.raw.block_hash, hexBytes(0x7F, 32), 1'700'000'000, 1'000),
         makeBlockInfo(10, removed.raw.block_hash, hexBytes(0x7F, 32), 1'700'000'010, 1'000)},
        11, 1'000, events::ScannedBlockRange{.from_block = 5, .to_block = 10}));

    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {}, {}, {}, 11, 2'000,
        events::ScannedBlockRange{.from_block = 9, .to_block = 10}));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot WHERE block_number=5;"), "observed");
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot WHERE block_number=10;"), "removed");
}

TEST_F(UnitTest, Rpc_HotStore_LocalAndRpcSourcesKeepIndependentCursorsAndShareChangeSeq)
{
    asio::io_context io_context;
    const auto paths = makeTempEventsPaths("rpc_local_source_isolation");

    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent local_event = makeRemoteEvent(3, 0x33);
    const events::DecodedEvent remote_event = makeRemoteEvent(7, 0x77);

    ASSERT_TRUE(store.ingestBatch(
        "local", events::CursorKind::LOCAL_SEQUENCE, CHAIN_ID, {local_event.raw}, {local_event},
        {makeBlockInfo(3, local_event.raw.block_hash, hexBytes(0x7F, 32), 1'700'000'000, 1'000)},
        9, 1'000, std::nullopt));

    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {remote_event.raw}, {remote_event},
        {makeBlockInfo(7, remote_event.raw.block_hash, hexBytes(0x7F, 32), 1'700'000'010, 1'000)},
        8, 1'000, events::ScannedBlockRange{.from_block = 7, .to_block = 7}));

    // Same chain id, two logical sources: cursors are per source and the change stream
    // stays a single global ordering.
    EXPECT_EQ(store.loadNextCursor("local", events::CursorKind::LOCAL_SEQUENCE).value_or(0), 9u);
    EXPECT_EQ(store.loadNextCursor(RPC_SOURCE, events::CursorKind::NEXT_BLOCK).value_or(0), 8u);

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM source_resume;"), 2);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(DISTINCT change_seq) FROM normalized_events_hot;"), 2);
}

TEST_F(UnitTest, Rpc_HotStore_RescanRestoresALogItWronglyRemoved)
{
    asio::io_context io_context;
    const auto paths = makeTempEventsPaths("rpc_scan_resurrects_log");

    events::SQLiteHotStore store(paths.hot_db, CHAIN_ID);

    const events::DecodedEvent event = makeRemoteEvent(10, 0x10);
    const events::ChainBlockInfo block =
        makeBlockInfo(10, event.raw.block_hash, hexBytes(0x7F, 32), 1'700'000'000, 1'000);
    const events::ScannedBlockRange range{.from_block = 10, .to_block = 10};

    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {event.raw}, {event}, {block}, 11, 1'000, range));
    // A lagging provider node answers the rescan without the log.
    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {}, {}, {}, 11, 2'000, range));
    {
        SqliteReadonly db(paths.hot_db);
        ASSERT_EQ(db.scalarText("SELECT state FROM normalized_events_hot;"), "removed");
    }

    // The next cycle sees it again in the same block: the row comes back, not a duplicate.
    ASSERT_TRUE(store.ingestBatch(
        RPC_SOURCE, events::CursorKind::NEXT_BLOCK, CHAIN_ID, {event.raw}, {event}, {block}, 11, 3'000, range));

    SqliteReadonly db(paths.hot_db);
    EXPECT_EQ(db.scalarText("SELECT state FROM normalized_events_hot;"), "observed");
    EXPECT_EQ(db.scalarInt64("SELECT removed FROM raw_events_hot;"), 0);
    EXPECT_EQ(db.scalarInt64("SELECT COUNT(1) FROM normalized_events_hot;"), 1);
}
