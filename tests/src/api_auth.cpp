#include "unit-tests.hpp"
#include "events_test_harness.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    // Hardhat's first development account.
    constexpr const char * WALLET_KEY = "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    constexpr const char * WALLET_ADDRESS = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";

    http::Request withAuthorization(const std::string & value)
    {
        http::Request request;
        request.setMethod(http::Method::POST).setPath(http::URL("/connector")).setVersion("HTTP/1.1");
        request.setHeader(http::Header::Authorization, value);
        return request;
    }

    std::shared_ptr<publish::Signer> makeWallet()
    {
        const std::filesystem::path key_path = buildPath() / std::format("api_auth_{}.key", currentTestName());
        std::ofstream out(key_path, std::ios::trunc);
        out << WALLET_KEY;
        out.close();
        std::filesystem::permissions(
            key_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);

        auto wallet = publish::Signer::fromKeyFile(key_path);
        if(!wallet) throw std::runtime_error(wallet.error());
        return *wallet;
    }

    // What a wallet's personal_sign returns for `message`.
    std::string personalSign(const publish::Signer & wallet, const std::string & message)
    {
        const auto digest = chain::keccakString(
            std::format("\x19" "Ethereum Signed Message:\n{}{}", message.size(), message));
        const auto signature = wallet.signDigest(digest);
        if(!signature) throw std::runtime_error(signature.error());
        return chain::bytesToHex(*signature);
    }

    http::Request nonceRequest(const std::string & origin)
    {
        http::Request request;
        request.setMethod(http::Method::GET).setPath(http::URL("/nonce")).setVersion("HTTP/1.1");
        if(!origin.empty()) request.setHeader(http::Header::Origin, origin);
        return request;
    }

    server::RouteArg addressArg(const std::string & address)
    {
        return server::RouteArg(
            server::RouteArgDef(server::RouteArgType::string, server::RouteArgRequirement::required), address);
    }

    http::Request authRequest(const json & body)
    {
        http::Request request;
        request.setMethod(http::Method::POST).setPath(http::URL("/auth")).setVersion("HTTP/1.1").setBody(body.dump());
        return request;
    }

    auth::LoginSettings siteSettings()
    {
        return auth::LoginSettings{
            .origins = {"https://decentralised.art", "http://localhost:54321"},
            .chain_id = 11155111};
    }
}

TEST_F(UnitTest, Chain_AddressToChecksumHex_MatchesTheEip55Vectors)
{
    for(const std::string expected : {
            "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
            "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
            "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB",
            "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb"})
    {
        const auto address = evmc::from_hex<chain::Address>(expected);
        ASSERT_TRUE(address.has_value()) << expected;
        EXPECT_EQ(chain::addressToChecksumHex(*address), expected);
    }
}

TEST_F(UnitTest, Auth_LoginOrigins_AreStoredCanonicallySoTheBrowserOriginMatches)
{
    asio::io_context io_context;
    // Accepted, but not as a browser serializes it.
    auth::AuthManager auth_manager(io_context, auth::LoginSettings{.origins = {"HTTPS://Example.COM:443"}});

    EXPECT_EQ(auth_manager.loginOrigin("https://example.com"), std::optional<std::string>("https://example.com"));
    EXPECT_EQ(auth_manager.loginOrigin("https://EXAMPLE.com:443"), std::optional<std::string>("https://example.com"));
    EXPECT_FALSE(auth_manager.loginOrigin("http://example.com").has_value());

    server::QueryArgsList query_args;
    query_args.emplace("origin", makeStringQueryArg("https://example.com"));
    const auto response = runAwaitable(io_context, GET_nonce(nonceRequest(""), {addressArg(WALLET_ADDRESS)}, std::move(query_args), auth_manager));
    ASSERT_EQ(response.getCode(), http::Code::OK) << response.getBody();
    const std::string message = json::parse(response.getBody()).at("message").get<std::string>();
    EXPECT_TRUE(message.starts_with("https://example.com wants you to sign in")) << message;
    EXPECT_NE(message.find("\nURI: https://example.com\n"), std::string::npos) << message;

    for(const std::vector<std::string> & origins : std::vector<std::vector<std::string>>{{}, {"https://:"}, {"https://example.com", "example.com"}})
    {
        EXPECT_THROW(auth::AuthManager(io_context, auth::LoginSettings{.origins = origins}), std::invalid_argument);
    }
}

TEST_F(UnitTest, API_Nonce_IssuesAnEip4361MessageNamingOneOfTheConfiguredOrigins)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context, siteSettings());

    const auto issue = [&](const std::string & origin)
    {
        const auto response = runAwaitable(io_context, GET_nonce(nonceRequest(origin), {addressArg(WALLET_ADDRESS)}, {}, auth_manager));
        EXPECT_EQ(response.getCode(), http::Code::OK);
        return json::parse(response.getBody());
    };

    const json body = issue("http://localhost:54321");
    const std::string nonce = body.at("nonce").get<std::string>();
    const std::string message = body.at("message").get<std::string>();
    EXPECT_EQ(nonce.size(), 16U);
    EXPECT_TRUE(std::ranges::all_of(nonce, [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }));

    EXPECT_TRUE(message.starts_with(std::format(
        "http://localhost:54321 wants you to sign in with your Ethereum account:\n{}\n\n"
        "Sign in to decentralised.art.\n\n"
        "URI: http://localhost:54321\n"
        "Version: 1\n"
        "Chain ID: 11155111\n"
        "Nonce: {}\n"
        "Issued At: ", WALLET_ADDRESS, nonce))) << message;
    EXPECT_NE(message.find("\nExpiration Time: "), std::string::npos) << message;

    // An origin that is not ours, or none at all, gets the first configured one.
    for(const std::string origin : {"https://evil.example", ""})
    {
        EXPECT_TRUE(issue(origin).at("message").get<std::string>().starts_with(
            "https://decentralised.art wants you to sign in")) << origin;
    }

    const auto bad = runAwaitable(io_context, GET_nonce(nonceRequest(""), {addressArg("0x1234")}, {}, auth_manager));
    EXPECT_EQ(bad.getCode(), http::Code::BadRequest);
}

TEST_F(UnitTest, API_Nonce_NamesTheSecondaryOriginAPageAsksForOnASameOriginRequest)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context, siteSettings());

    const auto nonce = [&](const std::string & origin_header, const std::optional<std::string> & origin_query)
    {
        server::QueryArgsList query_args;
        if(origin_query) query_args.emplace("origin", makeStringQueryArg(*origin_query));
        return runAwaitable(io_context, GET_nonce(nonceRequest(origin_header), {addressArg(WALLET_ADDRESS)}, std::move(query_args), auth_manager));
    };
    const auto first_line_of = [](const http::Response & response)
    {
        const std::string message = json::parse(response.getBody()).at("message").get<std::string>();
        return message.substr(0, message.find('\n'));
    };
    const std::string secondary = "http://localhost:54321 wants you to sign in with your Ethereum account:";

    // A same-origin GET carries no Origin header: the page on the secondary origin names it.
    const auto same_origin = nonce("", "http://localhost:54321");
    ASSERT_EQ(same_origin.getCode(), http::Code::OK) << same_origin.getBody();
    EXPECT_EQ(first_line_of(same_origin), secondary);

    // The explicit selector wins over the header.
    const auto both = nonce("https://decentralised.art", "http://localhost:54321");
    ASSERT_EQ(both.getCode(), http::Code::OK);
    EXPECT_EQ(first_line_of(both), secondary);

    // Without either, the first configured origin is named.
    const auto neither = nonce("", std::nullopt);
    ASSERT_EQ(neither.getCode(), http::Code::OK);
    EXPECT_EQ(first_line_of(neither), "https://decentralised.art wants you to sign in with your Ethereum account:");

    // A selector outside the allowlist is refused rather than replaced.
    for(const std::string origin : {"https://evil.example", "https://decentralised.art/", "localhost:54321", ""})
    {
        EXPECT_EQ(nonce("", origin).getCode(), http::Code::BadRequest) << origin;
    }
}

TEST_F(UnitTest, API_Auth_RedeemsASignedIssuedMessageOnce)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context, siteSettings());
    const auto wallet = makeWallet();
    ASSERT_EQ(chain::addressToChecksumHex(wallet->address()), WALLET_ADDRESS);

    const auto issued = runAwaitable(io_context, auth_manager.issueLogin(wallet->address(), "https://decentralised.art"));
    ASSERT_TRUE(issued.has_value());
    const json body{
        {"address", WALLET_ADDRESS},
        {"nonce", issued->nonce},
        {"signature", personalSign(*wallet, issued->message)}};

    const auto response = runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager));
    ASSERT_EQ(response.getCode(), http::Code::OK) << response.getBody();
    const std::string token = json::parse(response.getBody()).at("access_token").get<std::string>();
    const auto authenticated = runAwaitable(io_context, auth_manager.verifyAccessToken(token));
    ASSERT_TRUE(authenticated.has_value());
    EXPECT_EQ(*authenticated, wallet->address());

    // Replaying the same nonce and signature is refused.
    EXPECT_EQ(runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager)).getCode(), http::Code::BadRequest);
}

TEST_F(UnitTest, API_Auth_RefusesSignaturesOfAnythingButTheMessageIssuedToTheSigner)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context, siteSettings());
    const auto wallet = makeWallet();

    const auto refused = [&](const std::string & address, const std::string & nonce, const std::string & signed_message)
    {
        const json body{{"address", address}, {"nonce", nonce}, {"signature", personalSign(*wallet, signed_message)}};
        return runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager)).getCode() == http::Code::BadRequest;
    };
    const auto issue = [&](const chain::Address & address)
    {
        return runAwaitable(io_context, auth_manager.issueLogin(address, "")).value();
    };

    // The format this server used to accept.
    const auto legacy = issue(wallet->address());
    const std::string legacy_message = "Login nonce: " + legacy.nonce;
    EXPECT_TRUE(refused(WALLET_ADDRESS, legacy.nonce, legacy_message));

    // An issued message with a site the server did not name.
    const auto rewritten = issue(wallet->address());
    std::string phished = rewritten.message;
    phished.replace(0, std::string("https://decentralised.art").size(), "https://evil.example");
    EXPECT_TRUE(refused(WALLET_ADDRESS, rewritten.nonce, phished));

    // A message issued to another address, claimed by the signer or signed for that address.
    const chain::Address other = makeAddressFromByte(0x42);
    const auto claimed = issue(other);
    EXPECT_TRUE(refused(WALLET_ADDRESS, claimed.nonce, claimed.message));
    const auto impersonated = issue(other);
    EXPECT_TRUE(refused(chain::addressToChecksumHex(other), impersonated.nonce, impersonated.message));

    // A signature over a different message.
    const auto mismatched = issue(wallet->address());
    EXPECT_TRUE(refused(WALLET_ADDRESS, mismatched.nonce, mismatched.message + " "));

    // A nonce the server never issued.
    const auto unissued = issue(wallet->address());
    EXPECT_TRUE(refused(WALLET_ADDRESS, "0000000000000000", unissued.message));

    const auto missing = runAwaitable(io_context, POST_auth(authRequest(json{{"address", WALLET_ADDRESS}}), {}, {}, auth_manager));
    EXPECT_EQ(missing.getCode(), http::Code::BadRequest);
    http::Request not_json = authRequest(json::object());
    not_json.setBody("{");
    EXPECT_EQ(runAwaitable(io_context, POST_auth(not_json, {}, {}, auth_manager)).getCode(), http::Code::BadRequest);
}

TEST_F(UnitTest, Auth_IssueLogin_KeepsEarlierMessagesForTheSameAddressValid)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context, siteSettings());
    const auto wallet = makeWallet();

    const auto first = runAwaitable(io_context, auth_manager.issueLogin(wallet->address(), ""));
    // Anyone may ask for a message for any address; that must not void one already issued.
    (void)runAwaitable(io_context, auth_manager.issueLogin(wallet->address(), ""));

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(runAwaitable(io_context, auth_manager.consumeLogin(wallet->address(), first->nonce)), first->message);
}

TEST_F(UnitTest, Auth_IssueLogin_RefusesOncePendingMessagesReachTheCap)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    const chain::Address address = makeAddressFromByte(0x01);

    std::optional<auth::LoginChallenge> last;
    for(std::size_t i = 0; i < auth::MAX_PENDING_LOGINS; ++i)
    {
        last = runAwaitable(io_context, auth_manager.issueLogin(address, ""));
        ASSERT_TRUE(last.has_value()) << i;
    }
    EXPECT_FALSE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value());

    // Redeeming one frees its slot.
    EXPECT_TRUE(runAwaitable(io_context, auth_manager.consumeLogin(address, last->nonce)).has_value());
    EXPECT_TRUE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value());
}

TEST_F(UnitTest, API_Authenticate_ReadsTheBearerTokenAfterAnyWhitespace)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    const chain::Address owner = makeAddressFromByte(0x42);
    const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(owner));

    for(const std::string & header : {
            std::format("Bearer {}", token),
            std::format("Bearer \t {}", token),
            std::format("Bearer {} trailing", token),
            std::format("BearerX Bearer {}", token)})
    {
        const auto result = runAwaitable(io_context, authenticate(withAuthorization(header), auth_manager));
        ASSERT_TRUE(result.has_value()) << header;
        EXPECT_EQ(*result, owner);
    }

    for(const std::string & header : {std::string("Bearer"), std::string("Bearer   "), std::format("Bearer{}", token), token})
    {
        EXPECT_FALSE(runAwaitable(io_context, authenticate(withAuthorization(header), auth_manager)).has_value()) << header;
    }
}

TEST_F(UnitTest, API_Authenticate_RefusesAHeaderAtTheSizeLimitWithoutCrashing)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);

    const std::string header = "Bearer " + std::string(server::MAX_HEADER_SIZE, 'A');
    const auto result = runAwaitable(io_context, authenticate(withAuthorization(header), auth_manager));

    EXPECT_FALSE(result.has_value());
}

TEST_F(UnitTest, Auth_AccessToken_StaysValidWhenAnotherLoginSweepsExpiredTokens)
{
    // Tokens are issued in the past, never ahead of the wall clock their JWT is checked on.
    asio::io_context io_context;
    const auto wall_clock = std::chrono::system_clock::now();
    auto now = wall_clock - std::chrono::minutes{6};
    auth::AuthManager auth_manager(io_context, {}, [&now] { return now; });

    // Enough tokens that the next login sweeps; all but `live` are expired by then.
    std::vector<std::string> expired_tokens;
    for(std::size_t i = 0; i + 1 < auth::ACCESS_TOKEN_SWEEP_MIN; ++i)
    {
        chain::Address address{};
        std::memcpy(address.bytes, &i, sizeof(i));
        expired_tokens.push_back(runAwaitable(io_context, auth_manager.generateAccessToken(address)));
    }
    now = wall_clock - std::chrono::minutes{2};
    const chain::Address live = makeAddressFromByte(0xEE);
    const std::string live_token = runAwaitable(io_context, auth_manager.generateAccessToken(live));

    now = wall_clock;
    const chain::Address sweeper = makeAddressFromByte(0xEF);
    const std::string sweeper_token = runAwaitable(io_context, auth_manager.generateAccessToken(sweeper));

    const auto live_result = runAwaitable(io_context, auth_manager.verifyAccessToken(live_token));
    ASSERT_TRUE(live_result.has_value());
    EXPECT_EQ(*live_result, live);
    EXPECT_TRUE(runAwaitable(io_context, auth_manager.verifyAccessToken(sweeper_token)).has_value());
    EXPECT_FALSE(runAwaitable(io_context, auth_manager.verifyAccessToken(expired_tokens.front())).has_value());
    EXPECT_EQ(runAwaitable(io_context, auth_manager.storage()).access_tokens, 2U);
}

TEST_F(UnitTest, Auth_AccessToken_SweepsAndShrinksABurstOnceItExpiresEvenBelowTheSizeThreshold)
{
    asio::io_context io_context;
    const auto wall_clock = std::chrono::system_clock::now();
    auto now = wall_clock - std::chrono::minutes{6};
    auth::AuthManager auth_manager(io_context, {}, [&now] { return now; });
    const auto address_of = [](const std::size_t i)
    {
        chain::Address address{};
        std::memcpy(address.bytes, &i, sizeof(i));
        return address;
    };

    // The burst leaves the size threshold (8192) above what the map holds (6144), and the
    // table's capacity above the compaction floor.
    constexpr std::size_t burst = 6 * auth::ACCESS_TOKEN_SWEEP_MIN;
    for(std::size_t i = 0; i < burst; ++i)
    {
        (void)runAwaitable(io_context, auth_manager.generateAccessToken(address_of(i)));
    }
    const auto peak = runAwaitable(io_context, auth_manager.storage());
    ASSERT_EQ(peak.access_tokens, burst);
    ASSERT_GT(peak.access_token_capacity, 4 * auth::ACCESS_TOKEN_SWEEP_MIN);

    // Once the burst expired, a re-login by one of its addresses sweeps the rest and
    // releases the table built for it.
    now = wall_clock;
    const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(address_of(0)));
    const auto after = runAwaitable(io_context, auth_manager.storage());
    EXPECT_EQ(after.access_tokens, 1U);
    EXPECT_LT(after.access_token_capacity, auth::ACCESS_TOKEN_SWEEP_MIN);
    EXPECT_TRUE(runAwaitable(io_context, auth_manager.verifyAccessToken(token)).has_value());
}

TEST_F(UnitTest, Auth_AccessToken_ExpiresAfterItsTtl)
{
    // Issued in the past, so its JWT is valid on the wall clock and only the retained expiry refuses it.
    asio::io_context io_context;
    const auto wall_clock = std::chrono::system_clock::now();
    auto now = wall_clock - std::chrono::minutes{4};
    auth::AuthManager auth_manager(io_context, {}, [&now] { return now; });
    const chain::Address owner = makeAddressFromByte(0x42);

    const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(owner));
    now = wall_clock;
    EXPECT_TRUE(runAwaitable(io_context, auth_manager.verifyAccessToken(token)).has_value());
    now = wall_clock + std::chrono::minutes{1} + std::chrono::seconds{1};
    EXPECT_FALSE(runAwaitable(io_context, auth_manager.verifyAccessToken(token)).has_value());
}

TEST_F(UnitTest, API_Auth_RefusesAMessageRedeemedAfterItExpires)
{
    asio::io_context io_context;
    auto now = std::chrono::system_clock::now();
    auth::AuthManager auth_manager(io_context, siteSettings(), [&now] { return now; });
    const auto wallet = makeWallet();

    const auto issued = runAwaitable(io_context, auth_manager.issueLogin(wallet->address(), ""));
    ASSERT_TRUE(issued.has_value());
    now += auth::LOGIN_TTL;

    const json body{{"address", WALLET_ADDRESS}, {"nonce", issued->nonce}, {"signature", personalSign(*wallet, issued->message)}};
    EXPECT_EQ(runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager)).getCode(), http::Code::BadRequest);
}

TEST_F(UnitTest, API_Auth_RefusesAnInvalidRecoveryIdWithoutAborting)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context, siteSettings());
    const auto wallet = makeWallet();

    // 4..26 and 31..255 leave a recovery id outside 0..3 once 27 is taken off.
    for(const std::string v : {"04", "1a", "1f", "ff"})
    {
        const auto issued = runAwaitable(io_context, auth_manager.issueLogin(wallet->address(), ""));
        ASSERT_TRUE(issued.has_value());
        std::string signature = personalSign(*wallet, issued->message);
        signature.replace(signature.size() - 2, 2, v);

        const json body{{"address", WALLET_ADDRESS}, {"nonce", issued->nonce}, {"signature", signature}};
        const auto response = runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager));
        EXPECT_EQ(response.getCode(), http::Code::BadRequest) << v;
        EXPECT_EQ(json::parse(response.getBody()).at("message"), "Invalid signature") << v;
    }
}

TEST_F(UnitTest, Auth_IssueLogin_AdmitsAgainOnceSaturatingMessagesExpire)
{
    asio::io_context io_context;
    auto now = std::chrono::system_clock::now();
    auth::AuthManager auth_manager(io_context, {}, [&now] { return now; });
    const chain::Address address = makeAddressFromByte(0x01);

    const auto first = runAwaitable(io_context, auth_manager.issueLogin(address, ""));
    ASSERT_TRUE(first.has_value());
    now += std::chrono::minutes{1};
    for(std::size_t i = 1; i < auth::MAX_PENDING_LOGINS; ++i)
    {
        ASSERT_TRUE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value()) << i;
    }

    // Saturated: refused until the oldest message expires, then admitted again.
    EXPECT_FALSE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value());
    now += auth::LOGIN_TTL - std::chrono::minutes{1} - std::chrono::seconds{1};
    EXPECT_FALSE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value());
    now += std::chrono::seconds{1};
    const auto admitted = runAwaitable(io_context, auth_manager.issueLogin(address, ""));
    ASSERT_TRUE(admitted.has_value());

    // The expired message is gone; the newer ones are still redeemable.
    EXPECT_FALSE(runAwaitable(io_context, auth_manager.consumeLogin(address, first->nonce)).has_value());
    EXPECT_TRUE(runAwaitable(io_context, auth_manager.consumeLogin(address, admitted->nonce)).has_value());
}

TEST_F(UnitTest, Auth_AccessToken_IsSignedWithAPerInstanceSecret)
{
    asio::io_context io_context;
    auth::AuthManager issuer(io_context);
    auth::AuthManager other(io_context);
    const chain::Address owner = makeAddressFromByte(0x07);

    const std::string token = runAwaitable(io_context, issuer.generateAccessToken(owner));
    // Registers a different token for the owner on `other`, so only the signature can refuse it.
    (void)runAwaitable(io_context, other.generateAccessToken(owner));

    EXPECT_FALSE(runAwaitable(io_context, other.verifyAccessToken(token)).has_value());
}

TEST_F(UnitTest, Auth_IssueLogin_RecoversFromRepeatedSaturation)
{
    asio::io_context io_context;
    auto now = std::chrono::system_clock::now();
    auth::AuthManager auth_manager(io_context, {}, [&now] { return now; });
    const chain::Address address = makeAddressFromByte(0x01);
    const auto fill = [&](const std::size_t count)
    {
        for(std::size_t i = 0; i < count; ++i)
        {
            ASSERT_TRUE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value()) << i;
        }
        EXPECT_FALSE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value());
    };

    fill(auth::MAX_PENDING_LOGINS);
    for(int round = 0; round < 2; ++round)
    {
        // Everything pending expires; the sweep that empties the map must not stop the next one.
        now += auth::LOGIN_TTL;
        ASSERT_TRUE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value()) << round;
        fill(auth::MAX_PENDING_LOGINS - 1);
    }
}

TEST_F(UnitTest, Auth_IssueLogin_ReclaimsStaggeredExpiriesOneAtATime)
{
    asio::io_context io_context;
    const auto start = std::chrono::system_clock::now();
    auto now = start;
    auth::AuthManager auth_manager(io_context, {}, [&now] { return now; });
    const chain::Address address = makeAddressFromByte(0x01);

    // One message per millisecond, so they expire one by one.
    for(std::size_t i = 0; i < auth::MAX_PENDING_LOGINS; ++i)
    {
        ASSERT_TRUE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value()) << i;
        now += std::chrono::milliseconds{1};
    }
    EXPECT_FALSE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value());

    // Each expiry frees exactly one slot.
    now = start + auth::LOGIN_TTL;
    for(int step = 0; step < 3; ++step)
    {
        EXPECT_TRUE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value()) << step;
        EXPECT_FALSE(runAwaitable(io_context, auth_manager.issueLogin(address, "")).has_value()) << step;
        now += std::chrono::milliseconds{1};
    }

    const auto storage = runAwaitable(io_context, auth_manager.storage());
    EXPECT_EQ(storage.pending_logins, auth::MAX_PENDING_LOGINS);
    EXPECT_EQ(storage.pending_login_queue, auth::MAX_PENDING_LOGINS);
}

TEST_F(UnitTest, Auth_IssueLogin_DropsRedeemedNoncesFromTheExpiryQueue)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    const chain::Address address = makeAddressFromByte(0x01);

    // Redeemed nonces stay queued until the queue passes twice the cap, then are dropped.
    for(std::size_t i = 0; i <= 2 * auth::MAX_PENDING_LOGINS; ++i)
    {
        const auto issued = runAwaitable(io_context, auth_manager.issueLogin(address, ""));
        ASSERT_TRUE(issued.has_value()) << i;
        ASSERT_TRUE(runAwaitable(io_context, auth_manager.consumeLogin(address, issued->nonce)).has_value()) << i;
    }

    const auto storage = runAwaitable(io_context, auth_manager.storage());
    EXPECT_EQ(storage.pending_logins, 0U);
    EXPECT_EQ(storage.pending_login_queue, 1U);
}
