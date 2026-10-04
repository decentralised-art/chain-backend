#include "unit-tests.hpp"
#include "test_connector_helpers.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
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
}

TEST_F(UnitTest, API_Format_Get_ReturnsPaginatedConnectorsForGivenHash)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xF0));

    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_hex, 0x11));
    ASSERT_TRUE(addScalarConnector(io_context, registry, "PITCH", owner_hex, 0x12));

    const chain::Address fmt_a_address = makeAddressFromByte(0x21);
    ConnectorRecord fmt_a = makeConnectorRecord("FMT_A", owner_hex);
    addDimension(fmt_a, "TIME");
    addDimension(fmt_a, "PITCH");
    ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(fmt_a_address, std::move(fmt_a))));

    const chain::Address fmt_b_address = makeAddressFromByte(0x22);
    ConnectorRecord fmt_b = makeConnectorRecord("FMT_B", owner_hex);
    addDimension(fmt_b, "TIME");
    addDimension(fmt_b, "PITCH");
    ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(fmt_b_address, std::move(fmt_b))));

    const chain::Address fmt_c_address = makeAddressFromByte(0x23);
    ConnectorRecord fmt_c = makeConnectorRecord("FMT_C", owner_hex);
    addDimension(fmt_c, "TIME");
    addDimension(fmt_c, "PITCH");
    ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(fmt_c_address, std::move(fmt_c))));

    const chain::Address other_address = makeAddressFromByte(0x24);
    ConnectorRecord other = makeConnectorRecord("FMT_OTHER", owner_hex);
    addDimension(other, "TIME");
    ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(other_address, std::move(other))));

    const auto fmt_a_hash = runAwaitable(io_context, registry.getFormatHash("FMT_A"));
    ASSERT_TRUE(fmt_a_hash.has_value());
    const std::string format_hash_hex = evmc::hex(*fmt_a_hash);

    {
        http::Request request;
        request.setMethod(http::Method::GET)
               .setPath(http::URL("/format/" + format_hash_hex + "?limit=2"))
               .setVersion("HTTP/1.1");

        std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(2));

        const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);

        const auto body = nlohmann::json::parse(response.getBody());
        EXPECT_EQ(body["format_hash"], format_hash_hex);
        EXPECT_EQ(body["limit"], 2);
        EXPECT_EQ(body["total_connectors"], 3);
        ASSERT_TRUE(body["cursor"].is_object());
        EXPECT_EQ(body["cursor"]["has_more"], true);
        EXPECT_EQ(body["cursor"]["next_after"], "FMT_B");
        ASSERT_TRUE(body["scalars"].is_array());
        ASSERT_EQ(body["scalars"].size(), 2);
        EXPECT_EQ(body["scalars"][0], "PITCH:0");
        EXPECT_EQ(body["scalars"][1], "TIME:0");
        EXPECT_FALSE(body.contains("scalar_labels"));

        ASSERT_TRUE(body["connectors"].is_array());
        ASSERT_EQ(body["connectors"].size(), 2);
        EXPECT_EQ(body["connectors"][0], "FMT_A");
        EXPECT_EQ(body["connectors"][1], "FMT_B");
    }

    {
        http::Request request;
        request.setMethod(http::Method::GET)
               .setPath(http::URL("/format/" + format_hash_hex + "?limit=2&after=FMT_B"))
               .setVersion("HTTP/1.1");

        std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(2));
        query_args.emplace("after", makeStringRouteArg("FMT_B"));

        const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);

        const auto body = nlohmann::json::parse(response.getBody());
        EXPECT_EQ(body["total_connectors"], 3);
        ASSERT_TRUE(body["cursor"].is_object());
        EXPECT_EQ(body["cursor"]["has_more"], false);
        EXPECT_EQ(body["cursor"]["next_after"], nullptr);
        ASSERT_EQ(body["connectors"].size(), 1);
        EXPECT_EQ(body["connectors"][0], "FMT_C");
    }

    {
        http::Request request;
        request.setMethod(http::Method::GET)
               .setPath(http::URL("/format/" + format_hash_hex + "?limit=2&after=%20FMT_B%20"))
               .setVersion("HTTP/1.1");

        std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(2));
        query_args.emplace("after", makeStringRouteArg(" FMT_B "));

        const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);

        const auto body = nlohmann::json::parse(response.getBody());
        EXPECT_EQ(body["total_connectors"], 3);
        ASSERT_TRUE(body["cursor"].is_object());
        EXPECT_EQ(body["cursor"]["has_more"], false);
        EXPECT_EQ(body["cursor"]["next_after"], nullptr);
        ASSERT_EQ(body["connectors"].size(), 1);
        EXPECT_EQ(body["connectors"][0], "FMT_C");
    }
}

TEST_F(UnitTest, API_Format_Get_UnknownHashReturnsEmptyConnectorsList)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    evmc::bytes32 unknown_hash{};
    const std::string format_hash_hex = evmc::hex(unknown_hash);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/" + format_hash_hex + "?limit=10"))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::OK);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["format_hash"], format_hash_hex);
    EXPECT_EQ(body["limit"], 10);
    EXPECT_EQ(body["total_connectors"], 0);
    ASSERT_TRUE(body["cursor"].is_object());
    EXPECT_EQ(body["cursor"]["has_more"], false);
    EXPECT_EQ(body["cursor"]["next_after"], nullptr);
    ASSERT_TRUE(body["scalars"].is_array());
    EXPECT_TRUE(body["scalars"].empty());
    EXPECT_FALSE(body.contains("scalar_labels"));
    ASSERT_TRUE(body["connectors"].is_array());
    EXPECT_TRUE(body["connectors"].empty());
}

TEST_F(UnitTest, API_Format_Get_InvalidFormatHashArgumentReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/123?limit=10"))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeUintRouteArg(123)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid format hash argument");
}

TEST_F(UnitTest, API_Format_Get_InvalidFormatHashReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string invalid_format_hash = "not-a-bytes32";

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/" + invalid_format_hash + "?limit=10"))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(invalid_format_hash)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid format hash");
}

TEST_F(UnitTest, API_Format_Get_MissingLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    evmc::bytes32 unknown_hash{};
    const std::string format_hash_hex = evmc::hex(unknown_hash);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/" + format_hash_hex))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
    server::QueryArgsList query_args;

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Missing argument limit");
}

TEST_F(UnitTest, API_Format_Get_LimitAboveMaxReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    evmc::bytes32 unknown_hash{};
    const std::string format_hash_hex = evmc::hex(unknown_hash);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/" + format_hash_hex + "?limit=257"))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(257));

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid argument limit. limit error: Out of range.");
}

TEST_F(UnitTest, API_Format_Get_ZeroLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    evmc::bytes32 unknown_hash{};
    const std::string format_hash_hex = evmc::hex(unknown_hash);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/" + format_hash_hex + "?limit=0"))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(0));

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid argument limit. limit error: Out of range.");
}

TEST_F(UnitTest, API_Format_Get_InvalidAfterCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    evmc::bytes32 unknown_hash{};
    const std::string format_hash_hex = evmc::hex(unknown_hash);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/" + format_hash_hex + "?limit=10&after="))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after", makeStringRouteArg(""));

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after cursor");
}

TEST_F(UnitTest, API_Format_Get_WhitespaceOnlyAfterCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    evmc::bytes32 unknown_hash{};
    const std::string format_hash_hex = evmc::hex(unknown_hash);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/format/" + format_hash_hex + "?limit=10&after=%20%20"))
           .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(format_hash_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after", makeStringRouteArg("  "));

    const auto response = runAwaitable(io_context, GET_format(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);

    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after cursor");
}
