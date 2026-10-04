#include "unit-tests.hpp"
#include "test_connector_helpers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::tests;

namespace
{
    using dcn::tests::helpers::addDimension;
    using dcn::tests::helpers::addScalarConnector;
    using dcn::tests::helpers::makeAddressFromByte;
    using dcn::tests::helpers::makeConnectorRecord;
    using dcn::tests::helpers::runAwaitable;

    server::RouteArg makeStringRouteArg(const std::string & value)
    {
        return server::RouteArg(
            server::RouteArgDef(server::RouteArgType::string, server::RouteArgRequirement::required),
            value);
    }

    server::RouteArg makeUintRouteArg(std::size_t value)
    {
        return server::RouteArg(
            server::RouteArgDef(server::RouteArgType::unsigned_integer, server::RouteArgRequirement::required),
            std::to_string(value));
    }

    std::string toUpper(std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](const unsigned char ch)
            {
                return static_cast<char>(std::toupper(ch));
            });
        return value;
    }
}

TEST_F(UnitTest, API_Formats_Get_ReturnsPaginatedFormatHashes)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA1));

    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x11));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "PITCH", owner_hex, 0x12));

    {
        ConnectorRecord fmt_a = makeConnectorRecord("FMT_A", owner_hex);
        addDimension(fmt_a, "TIME");
        addDimension(fmt_a, "PITCH");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0x21), std::move(fmt_a))));
    }
    {
        ConnectorRecord fmt_b = makeConnectorRecord("FMT_B", owner_hex);
        addDimension(fmt_b, "TIME");
        addDimension(fmt_b, "PITCH");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0x22), std::move(fmt_b))));
    }
    {
        ConnectorRecord fmt_c = makeConnectorRecord("FMT_C", owner_hex);
        addDimension(fmt_c, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0x23), std::move(fmt_c))));
    }
    {
        ConnectorRecord fmt_d = makeConnectorRecord("FMT_D", owner_hex);
        addDimension(fmt_d, "PITCH");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0x24), std::move(fmt_d))));
    }

    std::vector<std::string> expected_hashes;
    const auto fmt_a_hash = runAwaitable(io_context, registry.getFormatHash("FMT_A"));
    const auto fmt_c_hash = runAwaitable(io_context, registry.getFormatHash("FMT_C"));
    const auto fmt_d_hash = runAwaitable(io_context, registry.getFormatHash("FMT_D"));
    ASSERT_TRUE(fmt_a_hash.has_value());
    ASSERT_TRUE(fmt_c_hash.has_value());
    ASSERT_TRUE(fmt_d_hash.has_value());
    expected_hashes.push_back(evmc::hex(*fmt_a_hash));
    expected_hashes.push_back(evmc::hex(*fmt_c_hash));
    expected_hashes.push_back(evmc::hex(*fmt_d_hash));
    std::sort(expected_hashes.begin(), expected_hashes.end());

    {
        http::Request request;
        request.setMethod(http::Method::GET)
               .setPath(http::URL("/formats?limit=2"))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(2));

        const auto response = runAwaitable(io_context, GET_formats(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);
        const auto body = nlohmann::json::parse(response.getBody());

        EXPECT_EQ(body["limit"], 2);
        EXPECT_EQ(body["total_formats"], 3);
        ASSERT_TRUE(body["cursor"].is_object());
        EXPECT_EQ(body["cursor"]["has_more"], true);
        EXPECT_EQ(body["cursor"]["next_after"], expected_hashes[1]);

        ASSERT_TRUE(body["formats"].is_array());
        ASSERT_EQ(body["formats"].size(), 2);
        EXPECT_EQ(body["formats"][0], expected_hashes[0]);
        EXPECT_EQ(body["formats"][1], expected_hashes[1]);
    }

    {
        http::Request request;
        request.setMethod(http::Method::GET)
               .setPath(http::URL("/formats?limit=2&after=" + expected_hashes[1]))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(2));
        query_args.emplace("after", makeStringRouteArg(expected_hashes[1]));

        const auto response = runAwaitable(io_context, GET_formats(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);
        const auto body = nlohmann::json::parse(response.getBody());

        EXPECT_EQ(body["total_formats"], 3);
        ASSERT_TRUE(body["cursor"].is_object());
        EXPECT_EQ(body["cursor"]["has_more"], false);
        EXPECT_EQ(body["cursor"]["next_after"], nullptr);

        ASSERT_TRUE(body["formats"].is_array());
        ASSERT_EQ(body["formats"].size(), 1);
        EXPECT_EQ(body["formats"][0], expected_hashes[2]);
    }
}

TEST_F(UnitTest, API_Formats_Get_AfterCursorAcceptsTrimmedPrefixedUppercaseHex)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xB1));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x31));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "PITCH", owner_hex, 0x32));

    {
        ConnectorRecord fmt_a = makeConnectorRecord("FMT_A", owner_hex);
        addDimension(fmt_a, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0x41), std::move(fmt_a))));
    }
    {
        ConnectorRecord fmt_b = makeConnectorRecord("FMT_B", owner_hex);
        addDimension(fmt_b, "PITCH");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0x42), std::move(fmt_b))));
    }

    const auto fmt_a_hash = runAwaitable(io_context, registry.getFormatHash("FMT_A"));
    const auto fmt_b_hash = runAwaitable(io_context, registry.getFormatHash("FMT_B"));
    ASSERT_TRUE(fmt_a_hash.has_value());
    ASSERT_TRUE(fmt_b_hash.has_value());

    std::vector<std::string> expected_hashes{
        evmc::hex(*fmt_a_hash),
        evmc::hex(*fmt_b_hash)};
    std::sort(expected_hashes.begin(), expected_hashes.end());

    const std::string after_token = " 0X" + toUpper(expected_hashes[0]) + " ";

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/formats?limit=10&after=" + after_token))
           .setVersion("HTTP/1.1");

    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after", makeStringRouteArg(after_token));

    const auto response = runAwaitable(io_context, GET_formats(request, {}, std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::OK);
    const auto body = nlohmann::json::parse(response.getBody());

    EXPECT_EQ(body["total_formats"], 2);
    ASSERT_TRUE(body["formats"].is_array());
    ASSERT_EQ(body["formats"].size(), 1);
    EXPECT_EQ(body["formats"][0], expected_hashes[1]);
}

TEST_F(UnitTest, API_Formats_Get_MissingLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/formats"))
           .setVersion("HTTP/1.1");

    const auto response = runAwaitable(io_context, GET_formats(request, {}, {}, registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Missing argument limit");
}

TEST_F(UnitTest, API_Formats_Get_InvalidAfterCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/formats?limit=10&after="))
           .setVersion("HTTP/1.1");

    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after", makeStringRouteArg(""));

    const auto response = runAwaitable(io_context, GET_formats(request, {}, std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after cursor");
}

TEST_F(UnitTest, API_Formats_Get_ZeroLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/formats?limit=0"))
           .setVersion("HTTP/1.1");

    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(0));

    const auto response = runAwaitable(io_context, GET_formats(request, {}, std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid argument limit. limit error: Out of range.");
}

TEST_F(UnitTest, API_Formats_Head_MirrorsValidationAndReturnsNoBody)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    {
        http::Request request;
        request.setMethod(http::Method::HEAD)
               .setPath(http::URL("/formats?limit=5"))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(5));

        const auto response = runAwaitable(io_context, HEAD_formats(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);
        EXPECT_TRUE(response.getBody().empty());
    }

    {
        http::Request request;
        request.setMethod(http::Method::HEAD)
               .setPath(http::URL("/formats"))
               .setVersion("HTTP/1.1");

        const auto response = runAwaitable(io_context, HEAD_formats(request, {}, {}, registry));
        ASSERT_EQ(response.getCode(), http::Code::BadRequest);
        EXPECT_TRUE(response.getBody().empty());
    }

    {
        http::Request request;
        request.setMethod(http::Method::HEAD)
               .setPath(http::URL("/formats?limit=0"))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(0));

        const auto response = runAwaitable(io_context, HEAD_formats(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::BadRequest);
        EXPECT_TRUE(response.getBody().empty());
    }
}
