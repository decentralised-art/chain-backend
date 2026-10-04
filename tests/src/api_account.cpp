#include "unit-tests.hpp"
#include "test_connector_helpers.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
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
}

TEST_F(UnitTest, API_Account_Get_InvalidAddressArgumentReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/123?limit=10"))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeUintRouteArg(123)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid address argument");
}

TEST_F(UnitTest, API_Account_Get_MissingLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA1));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Missing argument limit");
}

TEST_F(UnitTest, API_Account_Get_LimitOutOfRangeReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA2));

    for(const std::size_t limit : {std::size_t{0}, std::size_t{257}})
    {
        http::Request request;
        request.setMethod(http::Method::GET)
            .setPath(http::URL("/account/" + owner_hex + "?limit=" + std::to_string(limit)))
            .setVersion("HTTP/1.1");

        std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(limit));

        const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::BadRequest) << "limit=" << limit;
        const auto body = nlohmann::json::parse(response.getBody());
        EXPECT_EQ(body["message"], "Invalid argument limit. limit error: Out of range.");
    }
}

TEST_F(UnitTest, API_Account_Get_InvalidLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA5));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=invalid"))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeStringRouteArg("invalid"));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid argument limit. limit error: Type mismatch.");
}

TEST_F(UnitTest, API_Account_Get_InvalidAfterCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA3));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=10&after_connectors="))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after_connectors", makeStringRouteArg(""));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after_connectors cursor");
}

TEST_F(UnitTest, API_Account_Get_WhitespaceOnlyAfterCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA8));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=10&after_connectors=%20%20"))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after_connectors", makeStringRouteArg("  "));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after_connectors cursor");
}

TEST_F(UnitTest, API_Account_Get_InvalidAfterTransformationsCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA6));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=10&after_transformations="))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after_transformations", makeStringRouteArg(""));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after_transformations cursor");
}

TEST_F(UnitTest, API_Account_Get_WhitespaceOnlyAfterTransformationsCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA9));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=10&after_transformations=%20%20"))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after_transformations", makeStringRouteArg("  "));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after_transformations cursor");
}

TEST_F(UnitTest, API_Account_Get_InvalidAfterConditionsCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xA7));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=10&after_conditions="))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after_conditions", makeStringRouteArg(""));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after_conditions cursor");
}

TEST_F(UnitTest, API_Account_Get_WhitespaceOnlyAfterConditionsCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const std::string owner_hex = evmc::hex(makeAddressFromByte(0xAA));

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=10&after_conditions=%20%20"))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after_conditions", makeStringRouteArg("  "));

    const auto response = runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after_conditions cursor");
}

TEST_F(UnitTest, API_Account_Get_AfterConnectorsCursorIsTrimmed)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const chain::Address owner = makeAddressFromByte(0xAB);
    const std::string owner_hex = evmc::hex(owner);
    const std::string scalar_owner_hex = evmc::hex(makeAddressFromByte(0xE5));

    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", scalar_owner_hex, 0xC1));

    {
        ConnectorRecord alpha = makeConnectorRecord("ALPHA", owner_hex);
        addDimension(alpha, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0xC2), std::move(alpha))));
    }
    {
        ConnectorRecord beta = makeConnectorRecord("BETA", owner_hex);
        addDimension(beta, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0xC3), std::move(beta))));
    }

    http::Request request;
    request.setMethod(http::Method::GET)
        .setPath(http::URL("/account/" + owner_hex + "?limit=1&after_connectors=%20ALPHA%20"))
        .setVersion("HTTP/1.1");

    std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(1));
    query_args.emplace("after_connectors", makeStringRouteArg(" ALPHA "));

    const auto response =
        runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::OK);

    const auto body = nlohmann::json::parse(response.getBody());
    ASSERT_TRUE(body["owned_connectors"].is_array());
    ASSERT_EQ(body["owned_connectors"].size(), 1);
    EXPECT_EQ(body["owned_connectors"][0], "BETA");
    ASSERT_TRUE(body["cursor_connectors"].is_object());
    EXPECT_EQ(body["cursor_connectors"]["next_after"], nullptr);
}

TEST_F(UnitTest, API_Account_Get_NameCursorPaginationIsStableAcrossInsert)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const chain::Address owner = makeAddressFromByte(0xA4);
    const std::string owner_hex = evmc::hex(owner);
    const std::string scalar_owner_hex = evmc::hex(makeAddressFromByte(0xE4));

    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", scalar_owner_hex, 0xB1));

    {
        ConnectorRecord alpha_v1 = makeConnectorRecord("ALPHA", owner_hex);
        addDimension(alpha_v1, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0xB2), std::move(alpha_v1))));
    }
    {
        ConnectorRecord beta = makeConnectorRecord("BETA", owner_hex);
        addDimension(beta, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0xB3), std::move(beta))));
    }

    {
        http::Request request;
        request.setMethod(http::Method::GET)
            .setPath(http::URL("/account/" + owner_hex + "?limit=1"))
            .setVersion("HTTP/1.1");

        std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(1));

        const auto response =
            runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);

        const auto body = nlohmann::json::parse(response.getBody());
        ASSERT_TRUE(body["owned_connectors"].is_array());
        ASSERT_EQ(body["owned_connectors"].size(), 1);
        EXPECT_EQ(body["owned_connectors"][0], "ALPHA");
        ASSERT_TRUE(body["cursor_connectors"].is_object());
        EXPECT_EQ(body["cursor_connectors"]["has_more"], true);
        EXPECT_EQ(body["cursor_connectors"]["next_after"], "ALPHA");
    }

    {
        ConnectorRecord inserted_before_cursor = makeConnectorRecord("AARDVARK", owner_hex);
        addDimension(inserted_before_cursor, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0xB4), std::move(inserted_before_cursor))));
    }

    {
        http::Request request;
        request.setMethod(http::Method::GET)
            .setPath(http::URL("/account/" + owner_hex + "?limit=1&after_connectors=ALPHA"))
            .setVersion("HTTP/1.1");

        std::vector<server::RouteArg> args{makeStringRouteArg(owner_hex)};
        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(1));
        query_args.emplace("after_connectors", makeStringRouteArg("ALPHA"));

        const auto response =
            runAwaitable(io_context, GET_accountInfo(request, std::move(args), std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);

        const auto body = nlohmann::json::parse(response.getBody());
        ASSERT_TRUE(body["owned_connectors"].is_array());
        ASSERT_EQ(body["owned_connectors"].size(), 1);
        EXPECT_EQ(body["owned_connectors"][0], "BETA");
        ASSERT_TRUE(body["cursor_connectors"].is_object());
        EXPECT_EQ(body["cursor_connectors"]["has_more"], false);
        EXPECT_EQ(body["cursor_connectors"]["next_after"], nullptr);
    }
}
