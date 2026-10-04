#include "unit-tests.hpp"
#include "events_test_harness.hpp"
#include "crypto.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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

    // A nonce is 33 bytes of lower-case hex.
    constexpr std::size_t NONCE_LENGTH = 66;

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

    auth::LoginSettings siteSettings(std::string secret = {})
    {
        return auth::LoginSettings{
            .origins = {"https://decentralised.art", "http://localhost:54321"},
            .chain_id = 11155111,
            .secret = std::move(secret)};
    }

    json signedLogin(const publish::Signer & wallet, const auth::LoginChallenge & challenge)
    {
        return json{
            {"address", WALLET_ADDRESS},
            {"nonce", challenge.nonce},
            {"signature", personalSign(wallet, challenge.message)}};
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

TEST_F(UnitTest, Crypto_HmacSha256_MatchesRfc4231AndRandomBytesDiffer)
{
    // RFC 4231 test case 2.
    const auto mac = crypto::hmacSha256("Jefe", "what do ya want for nothing?");
    EXPECT_EQ(evmc::hex(evmc::bytes_view(mac.data(), mac.size())),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    const std::string first = crypto::randomBytes(32);
    EXPECT_EQ(first.size(), 32U);
    EXPECT_NE(first, crypto::randomBytes(32));

    EXPECT_TRUE(crypto::equalConstantTime("abc", "abc"));
    EXPECT_FALSE(crypto::equalConstantTime("abc", "abd"));
    EXPECT_FALSE(crypto::equalConstantTime("abc", "abcd"));
}

TEST_F(UnitTest, Auth_LoginSettings_AreCanonicalizedAndValidated)
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

    std::vector<std::string> too_many;
    for(std::size_t i = 0; i <= auth::MAX_LOGIN_ORIGINS; ++i) too_many.push_back(std::format("http://localhost:{}", i + 1));
    EXPECT_THROW(auth::AuthManager(io_context, auth::LoginSettings{.origins = too_many}), std::invalid_argument);
    too_many.pop_back();
    EXPECT_NO_THROW(auth::AuthManager(io_context, auth::LoginSettings{.origins = too_many}));

    EXPECT_THROW(auth::AuthManager(io_context, siteSettings(std::string(auth::MIN_SECRET_SIZE - 1, 's'))), std::invalid_argument);
    EXPECT_NO_THROW(auth::AuthManager(io_context, siteSettings(std::string(auth::MIN_SECRET_SIZE, 's'))));
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
    EXPECT_EQ(nonce.size(), NONCE_LENGTH);
    EXPECT_TRUE(std::ranges::all_of(nonce, [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) << nonce;

    EXPECT_TRUE(message.starts_with(std::format(
        "http://localhost:54321 wants you to sign in with your Ethereum account:\n{}\n\n"
        "Sign in to decentralised.art.\n\n"
        "URI: http://localhost:54321\n"
        "Version: 1\n"
        "Chain ID: 11155111\n"
        "Nonce: {}\n"
        "Issued At: ", WALLET_ADDRESS, nonce))) << message;
    EXPECT_NE(message.find("\nExpiration Time: "), std::string::npos) << message;

    // Each request gets its own nonce.
    EXPECT_NE(issue("http://localhost:54321").at("nonce"), nonce);

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

    const json body = signedLogin(*wallet, auth_manager.issueLogin(wallet->address(), "https://decentralised.art"));

    const auto response = runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager));
    ASSERT_EQ(response.getCode(), http::Code::OK) << response.getBody();
    // The token travels in the body only.
    EXPECT_TRUE(response.getHeader(http::Header::Authorization).empty());
    const std::string token = json::parse(response.getBody()).at("access_token").get<std::string>();
    const auto authenticated = auth_manager.verifyAccessToken(token);
    ASSERT_TRUE(authenticated.has_value());
    EXPECT_EQ(*authenticated, wallet->address());

    // Replaying the same nonce and signature is refused.
    const auto replay = runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager));
    EXPECT_EQ(replay.getCode(), http::Code::BadRequest);
    EXPECT_NE(json::parse(replay.getBody()).at("message").get<std::string>().find("nonce"), std::string::npos);
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
    const auto issue = [&](const chain::Address & address) { return auth_manager.issueLogin(address, ""); };

    // The format this server used to accept.
    const auto legacy = issue(wallet->address());
    EXPECT_TRUE(refused(WALLET_ADDRESS, legacy.nonce, "Login nonce: " + legacy.nonce));

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

    // A nonce the server never issued, or an issued one with any field altered.
    const auto unissued = issue(wallet->address());
    EXPECT_TRUE(refused(WALLET_ADDRESS, std::string(NONCE_LENGTH, '0'), unissued.message));
    EXPECT_TRUE(refused(WALLET_ADDRESS, "0000000000000000", unissued.message));
    for(const std::size_t position : {std::size_t{14}, std::size_t{17}, std::size_t{20}, NONCE_LENGTH - 1})
    {
        const auto tampered = issue(wallet->address());
        std::string nonce = tampered.nonce;
        nonce[position] = nonce[position] == '0' ? '1' : '0';
        EXPECT_TRUE(refused(WALLET_ADDRESS, nonce, tampered.message)) << position;
    }

    // Only the spelling the server issued.
    const auto upper = issue(wallet->address());
    std::string upper_nonce = upper.nonce;
    std::ranges::transform(upper_nonce, upper_nonce.begin(), [](const char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    if(upper_nonce != upper.nonce) EXPECT_TRUE(refused(WALLET_ADDRESS, upper_nonce, upper.message));

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

    const auto first = auth_manager.issueLogin(wallet->address(), "");
    // Anyone may ask for a message for any address; that must not void one already issued.
    const auto second = auth_manager.issueLogin(wallet->address(), "");

    for(const auto & challenge : {first, second})
    {
        const auto token = runAwaitable(io_context,
            auth_manager.login(wallet->address(), challenge.nonce, personalSign(*wallet, challenge.message)));
        EXPECT_TRUE(token.has_value());
    }
}

TEST_F(UnitTest, API_Auth_InstancesSharingTheSecretRedeemEachOthersNoncesAndTokens)
{
    asio::io_context io_context;
    const std::string secret(auth::MIN_SECRET_SIZE, 'k');
    auth::AuthManager issuer(io_context, siteSettings(secret));
    auth::AuthManager redeemer(io_context, siteSettings(secret));
    const auto wallet = makeWallet();

    const json body = signedLogin(*wallet, issuer.issueLogin(wallet->address(), "http://localhost:54321"));
    const auto response = runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, redeemer));
    ASSERT_EQ(response.getCode(), http::Code::OK) << response.getBody();

    const std::string token = json::parse(response.getBody()).at("access_token").get<std::string>();
    const auto on_issuer = issuer.verifyAccessToken(token);
    ASSERT_TRUE(on_issuer.has_value());
    EXPECT_EQ(*on_issuer, wallet->address());

    EXPECT_EQ(runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, redeemer)).getCode(), http::Code::BadRequest);
}

TEST_F(UnitTest, Auth_InstancesWithoutASharedSecretRefuseEachOthersNoncesAndTokens)
{
    asio::io_context io_context;
    auth::AuthManager issuer(io_context, siteSettings());
    auth::AuthManager other(io_context, siteSettings());
    const auto wallet = makeWallet();

    EXPECT_FALSE(other.verifyAccessToken(issuer.generateAccessToken(wallet->address())).has_value());

    const auto challenge = issuer.issueLogin(wallet->address(), "");
    const auto token = runAwaitable(io_context,
        other.login(wallet->address(), challenge.nonce, personalSign(*wallet, challenge.message)));
    ASSERT_FALSE(token.has_value());
    EXPECT_EQ(token.error().kind, auth::AuthError::Kind::INVALID_NONCE);
}

TEST_F(UnitTest, API_Authenticate_ReadsOneCaseInsensitiveBearerToken)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    const chain::Address owner = makeAddressFromByte(0x42);
    const std::string token = auth_manager.generateAccessToken(owner);

    for(const std::string & header : {
            std::format("Bearer {}", token),
            std::format("bearer {}", token),
            std::format("BEARER \t {}", token),
            std::format("  Bearer {}  ", token)})
    {
        const auto result = runAwaitable(io_context, authenticate(withAuthorization(header), auth_manager));
        ASSERT_TRUE(result.has_value()) << header;
        EXPECT_EQ(*result, owner);
    }

    for(const std::string & header : {
            std::string("Bearer"), std::string("Bearer   "), std::format("Bearer{}", token), token,
            std::format("Bearer {} trailing", token), std::format("BearerX Bearer {}", token),
            std::format("Basic Bearer {}", token), std::string("Basic dXNlcjpwYXNz")})
    {
        EXPECT_FALSE(runAwaitable(io_context, authenticate(withAuthorization(header), auth_manager)).has_value()) << header;
    }

    // Two Authorization headers are ambiguous, even when one carries a valid token.
    http::Request doubled = withAuthorization(std::format("Bearer {}", token));
    doubled.addHeader(http::Header::Authorization, "Basic dXNlcjpwYXNz");
    EXPECT_FALSE(runAwaitable(io_context, authenticate(doubled, auth_manager)).has_value());

    http::Request none;
    none.setMethod(http::Method::POST).setPath(http::URL("/connector")).setVersion("HTTP/1.1");
    EXPECT_FALSE(runAwaitable(io_context, authenticate(none, auth_manager)).has_value());
}

TEST_F(UnitTest, API_Authenticate_RefusesAHeaderAtTheSizeLimitWithoutCrashing)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);

    const std::string header = "Bearer " + std::string(server::MAX_HEADER_SIZE, 'A');
    const auto result = runAwaitable(io_context, authenticate(withAuthorization(header), auth_manager));

    EXPECT_FALSE(result.has_value());
}

TEST_F(UnitTest, Auth_AccessToken_ExpiresAfterItsTtl)
{
    asio::io_context io_context;
    const auto start = std::chrono::system_clock::now();
    auto now = start;
    auth::AuthManager auth_manager(io_context, {}, [&now] { return now; });
    const chain::Address owner = makeAddressFromByte(0x42);

    const std::string token = auth_manager.generateAccessToken(owner);
    now = start + auth::ACCESS_TOKEN_TTL - std::chrono::minutes{1};
    EXPECT_TRUE(auth_manager.verifyAccessToken(token).has_value());
    now = start + auth::ACCESS_TOKEN_TTL + std::chrono::seconds{1};
    EXPECT_FALSE(auth_manager.verifyAccessToken(token).has_value());

    // Nor is a token accepted before it was issued.
    now = start - std::chrono::minutes{1};
    EXPECT_FALSE(auth_manager.verifyAccessToken(token).has_value());

    EXPECT_FALSE(auth_manager.verifyAccessToken("").has_value());
    EXPECT_FALSE(auth_manager.verifyAccessToken("not.a.jwt").has_value());
}

TEST_F(UnitTest, API_Auth_RefusesAMessageRedeemedAfterItExpires)
{
    asio::io_context io_context;
    auto now = std::chrono::system_clock::now();
    auth::AuthManager auth_manager(io_context, siteSettings(), [&now] { return now; });
    const auto wallet = makeWallet();

    const json body = signedLogin(*wallet, auth_manager.issueLogin(wallet->address(), ""));
    now += auth::LOGIN_TTL;

    EXPECT_EQ(runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager)).getCode(), http::Code::BadRequest);
}

TEST_F(UnitTest, Auth_Login_KeepsRefusingARedeemedNonceUntilItExpires)
{
    asio::io_context io_context;
    const auto start = std::chrono::system_clock::now();
    auto now = start;
    auth::AuthManager auth_manager(io_context, siteSettings(), [&now] { return now; });
    const auto wallet = makeWallet();
    const auto login = [&](const auth::LoginChallenge & challenge)
    {
        return runAwaitable(io_context,
            auth_manager.login(wallet->address(), challenge.nonce, personalSign(*wallet, challenge.message)));
    };

    const auto early = auth_manager.issueLogin(wallet->address(), "");
    ASSERT_TRUE(login(early).has_value());

    now = start + auth::LOGIN_TTL - std::chrono::seconds{2};
    const auto late = auth_manager.issueLogin(wallet->address(), "");
    ASSERT_TRUE(login(late).has_value());
    EXPECT_FALSE(login(early).has_value());

    // Once `early` expires, the next login drops it from the redeemed set; `late` stays refused.
    now = start + auth::LOGIN_TTL + std::chrono::seconds{1};
    ASSERT_TRUE(login(auth_manager.issueLogin(wallet->address(), "")).has_value());
    const auto replay = login(late);
    ASSERT_FALSE(replay.has_value());
    EXPECT_EQ(replay.error().kind, auth::AuthError::Kind::INVALID_NONCE);
}

TEST_F(UnitTest, API_Auth_RefusesAnInvalidRecoveryIdWithoutAborting)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context, siteSettings());
    const auto wallet = makeWallet();

    // 4..26 and 31..255 leave a recovery id outside 0..3 once 27 is taken off.
    for(const std::string v : {"04", "1a", "1f", "ff"})
    {
        const auto issued = auth_manager.issueLogin(wallet->address(), "");
        std::string signature = personalSign(*wallet, issued.message);
        signature.replace(signature.size() - 2, 2, v);

        const json body{{"address", WALLET_ADDRESS}, {"nonce", issued.nonce}, {"signature", signature}};
        const auto response = runAwaitable(io_context, POST_auth(authRequest(body), {}, {}, auth_manager));
        EXPECT_EQ(response.getCode(), http::Code::BadRequest) << v;
        EXPECT_EQ(json::parse(response.getBody()).at("message"), "Invalid signature") << v;
    }
}
