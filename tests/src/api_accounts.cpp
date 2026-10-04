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

    TransformationRecord makeTransformationRecord(const std::string & name, const std::string & owner_hex)
    {
        TransformationRecord record;
        record.set_owner(owner_hex);
        record.mutable_transformation()->set_name(name);
        return record;
    }

    ConditionRecord makeConditionRecord(const std::string & name, const std::string & owner_hex)
    {
        ConditionRecord record;
        record.set_owner(owner_hex);
        record.mutable_condition()->set_name(name);
        return record;
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

TEST_F(UnitTest, API_Accounts_Get_ReturnsDistinctOwnersWithCursorPagination)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const chain::Address owner_a = makeAddressFromByte(0xA1);
    const chain::Address owner_b = makeAddressFromByte(0xB1);
    const chain::Address owner_c = makeAddressFromByte(0xC1);
    const std::string owner_a_hex = evmc::hex(owner_a);
    const std::string owner_b_hex = evmc::hex(owner_b);
    const std::string owner_c_hex = evmc::hex(owner_c);

    ASSERT_TRUE(addScalarConnector(io_context, registry, "TIME", owner_a_hex, 0x11));

    {
        ConnectorRecord connector = makeConnectorRecord("ACCT_CONN", owner_a_hex);
        addDimension(connector, "TIME");
        ASSERT_TRUE(runAwaitable(io_context, registry.addConnector(makeAddressFromByte(0x21), std::move(connector))));
    }
    ASSERT_TRUE(runAwaitable(
        io_context,
        registry.addTransformation(makeAddressFromByte(0x22), makeTransformationRecord("ACCT_TX_A", owner_a_hex))));
    ASSERT_TRUE(runAwaitable(
        io_context,
        registry.addTransformation(makeAddressFromByte(0x23), makeTransformationRecord("ACCT_TX_B", owner_b_hex))));
    ASSERT_TRUE(runAwaitable(
        io_context,
        registry.addCondition(makeAddressFromByte(0x24), makeConditionRecord("ACCT_COND_C", owner_c_hex))));

    std::vector<std::string> expected_accounts{owner_a_hex, owner_b_hex, owner_c_hex};
    std::sort(expected_accounts.begin(), expected_accounts.end());

    {
        http::Request request;
        request.setMethod(http::Method::GET)
               .setPath(http::URL("/accounts?limit=2"))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(2));

        const auto response = runAwaitable(io_context, GET_accounts(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);
        const auto body = nlohmann::json::parse(response.getBody());

        EXPECT_EQ(body["limit"], 2);
        EXPECT_EQ(body["total_accounts"], 3);
        ASSERT_TRUE(body["cursor"].is_object());
        EXPECT_EQ(body["cursor"]["has_more"], true);
        EXPECT_EQ(body["cursor"]["next_after"], expected_accounts[1]);

        ASSERT_TRUE(body["accounts"].is_array());
        ASSERT_EQ(body["accounts"].size(), 2);
        EXPECT_EQ(body["accounts"][0], expected_accounts[0]);
        EXPECT_EQ(body["accounts"][1], expected_accounts[1]);
    }

    {
        http::Request request;
        request.setMethod(http::Method::GET)
               .setPath(http::URL("/accounts?limit=2&after=" + expected_accounts[1]))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(2));
        query_args.emplace("after", makeStringRouteArg(expected_accounts[1]));

        const auto response = runAwaitable(io_context, GET_accounts(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);
        const auto body = nlohmann::json::parse(response.getBody());

        EXPECT_EQ(body["total_accounts"], 3);
        ASSERT_TRUE(body["cursor"].is_object());
        EXPECT_EQ(body["cursor"]["has_more"], false);
        EXPECT_EQ(body["cursor"]["next_after"], nullptr);

        ASSERT_TRUE(body["accounts"].is_array());
        ASSERT_EQ(body["accounts"].size(), 1);
        EXPECT_EQ(body["accounts"][0], expected_accounts[2]);
    }
}

TEST_F(UnitTest, API_Accounts_Get_AfterCursorAcceptsTrimmedPrefixedUppercaseHex)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    const chain::Address owner_a = makeAddressFromByte(0xD1);
    const chain::Address owner_b = makeAddressFromByte(0xE1);
    const std::string owner_a_hex = evmc::hex(owner_a);
    const std::string owner_b_hex = evmc::hex(owner_b);

    ASSERT_TRUE(runAwaitable(
        io_context,
        registry.addTransformation(makeAddressFromByte(0x31), makeTransformationRecord("ACCT_TX_A", owner_a_hex))));
    ASSERT_TRUE(runAwaitable(
        io_context,
        registry.addCondition(makeAddressFromByte(0x32), makeConditionRecord("ACCT_COND_B", owner_b_hex))));

    std::vector<std::string> expected_accounts{owner_a_hex, owner_b_hex};
    std::sort(expected_accounts.begin(), expected_accounts.end());

    const std::string after_token = " 0X" + toUpper(expected_accounts[0]) + " ";

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/accounts?limit=10&after=" + after_token))
           .setVersion("HTTP/1.1");

    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after", makeStringRouteArg(after_token));

    const auto response = runAwaitable(io_context, GET_accounts(request, {}, std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::OK);
    const auto body = nlohmann::json::parse(response.getBody());

    EXPECT_EQ(body["total_accounts"], 2);
    ASSERT_TRUE(body["accounts"].is_array());
    ASSERT_EQ(body["accounts"].size(), 1);
    EXPECT_EQ(body["accounts"][0], expected_accounts[1]);
}

TEST_F(UnitTest, API_Accounts_Get_MissingLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/accounts"))
           .setVersion("HTTP/1.1");

    const auto response = runAwaitable(io_context, GET_accounts(request, {}, {}, registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Missing argument limit");
}

TEST_F(UnitTest, API_Accounts_Get_InvalidAfterCursorReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/accounts?limit=10&after="))
           .setVersion("HTTP/1.1");

    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(10));
    query_args.emplace("after", makeStringRouteArg(""));

    const auto response = runAwaitable(io_context, GET_accounts(request, {}, std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid after cursor");
}

TEST_F(UnitTest, API_Accounts_Get_ZeroLimitReturnsBadRequest)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    http::Request request;
    request.setMethod(http::Method::GET)
           .setPath(http::URL("/accounts?limit=0"))
           .setVersion("HTTP/1.1");

    server::QueryArgsList query_args;
    query_args.emplace("limit", makeUintRouteArg(0));

    const auto response = runAwaitable(io_context, GET_accounts(request, {}, std::move(query_args), registry));
    ASSERT_EQ(response.getCode(), http::Code::BadRequest);
    const auto body = nlohmann::json::parse(response.getBody());
    EXPECT_EQ(body["message"], "Invalid argument limit. limit error: Out of range.");
}

TEST_F(UnitTest, API_Accounts_Head_MirrorsValidationAndReturnsNoBody)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    {
        http::Request request;
        request.setMethod(http::Method::HEAD)
               .setPath(http::URL("/accounts?limit=5"))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(5));

        const auto response = runAwaitable(io_context, HEAD_accounts(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::OK);
        EXPECT_TRUE(response.getBody().empty());
    }

    {
        http::Request request;
        request.setMethod(http::Method::HEAD)
               .setPath(http::URL("/accounts"))
               .setVersion("HTTP/1.1");

        const auto response = runAwaitable(io_context, HEAD_accounts(request, {}, {}, registry));
        ASSERT_EQ(response.getCode(), http::Code::BadRequest);
        EXPECT_TRUE(response.getBody().empty());
    }

    {
        http::Request request;
        request.setMethod(http::Method::HEAD)
               .setPath(http::URL("/accounts?limit=0"))
               .setVersion("HTTP/1.1");

        server::QueryArgsList query_args;
        query_args.emplace("limit", makeUintRouteArg(0));

        const auto response = runAwaitable(io_context, HEAD_accounts(request, {}, std::move(query_args), registry));
        ASSERT_EQ(response.getCode(), http::Code::BadRequest);
        EXPECT_TRUE(response.getBody().empty());
    }
}
