#include "auth.hpp"
#include "crypto.hpp"
#include "url.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>

namespace dcn::auth
{
    namespace
    {
        constexpr std::string_view LOGIN_STATEMENT = "Sign in to decentralised.art.";
        constexpr std::string_view TOKEN_ISSUER = "decentralised.art";

        // A nonce is the hex of issue time (8 bytes, big-endian Unix seconds), origin index (1),
        // random bytes (8) and the leading bytes of an HMAC over the address and those fields:
        // hex keeps it within EIP-4361's alphanumeric nonce.
        constexpr std::size_t NONCE_TIME_SIZE = 8;
        constexpr std::size_t NONCE_RANDOM_SIZE = 8;
        constexpr std::size_t NONCE_MAC_SIZE = 16;
        constexpr std::size_t NONCE_FIELDS_SIZE = NONCE_TIME_SIZE + 1 + NONCE_RANDOM_SIZE;
        constexpr std::size_t NONCE_SIZE = NONCE_FIELDS_SIZE + NONCE_MAC_SIZE;

        // Origins are stored as a browser serializes them, so they compare equal to the
        // `window.location.origin` or `Origin` a page sends.
        LoginSettings checkedSettings(LoginSettings settings)
        {
            if(settings.origins.empty()) throw std::invalid_argument("At least one sign-in origin is required");
            if(settings.origins.size() > MAX_LOGIN_ORIGINS)
            {
                throw std::invalid_argument(std::format("At most {} sign-in origins are supported", MAX_LOGIN_ORIGINS));
            }
            for(std::string & origin : settings.origins)
            {
                auto canonical = http::canonicalOrigin(origin);
                if(!canonical)
                {
                    throw std::invalid_argument(std::format(
                        "Sign-in origin '{}' is not an http(s)://host[:port] origin, e.g. https://decentralised.art", origin));
                }
                origin = std::move(*canonical);
            }

            if(settings.secret.empty())
            {
                settings.secret = crypto::randomBytes(MIN_SECRET_SIZE);
            }
            else if(settings.secret.size() < MIN_SECRET_SIZE)
            {
                throw std::invalid_argument(std::format("The auth secret must have at least {} bytes", MIN_SECRET_SIZE));
            }
            return settings;
        }

        std::string deriveKey(const std::string_view secret, const std::string_view purpose)
        {
            const auto key = crypto::hmacSha256(secret, purpose);
            return std::string(key.begin(), key.end());
        }

        std::string_view asChars(const std::span<const std::uint8_t> bytes)
        {
            return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        }

        // The MAC binds a nonce's fields to the address it was issued to.
        std::array<std::uint8_t, NONCE_MAC_SIZE> nonceMac(
            const std::string_view key, const chain::Address & address, const std::span<const std::uint8_t> fields)
        {
            std::string data(asChars(address.bytes));
            data += asChars(fields);
            const auto mac = crypto::hmacSha256(key, data);
            std::array<std::uint8_t, NONCE_MAC_SIZE> truncated{};
            std::copy_n(mac.begin(), truncated.size(), truncated.begin());
            return truncated;
        }

        // jwt-cpp reads the time from a clock object; this one reads the manager's time source.
        struct TokenClock
        {
            const std::function<std::chrono::system_clock::time_point()> * source;
            jwt::date now() const { return (*source)(); }
        };
    }

    AuthManager::AuthManager(
        asio::io_context & io_context,
        LoginSettings login_settings,
        std::function<std::chrono::system_clock::time_point()> now)
    :   _strand(asio::make_strand(io_context)),
        _login_settings(checkedSettings(std::move(login_settings))),
        _now(std::move(now)),
        _nonce_key(deriveKey(_login_settings.secret, "decentralised.art sign-in nonce")),
        _token_key(deriveKey(_login_settings.secret, "decentralised.art access token"))
    {
    }

    std::optional<std::string> AuthManager::loginOrigin(const std::string_view origin) const
    {
        const auto canonical = http::canonicalOrigin(origin);
        if(!canonical || std::ranges::find(_login_settings.origins, *canonical) == _login_settings.origins.end())
        {
            return std::nullopt;
        }
        return canonical;
    }

    std::string AuthManager::loginMessage(
        const chain::Address & address,
        const std::size_t origin_index,
        const std::string_view nonce,
        const std::chrono::sys_seconds issued_at) const
    {
        const std::string & uri = _login_settings.origins.at(origin_index);
        // The first line keeps the origin's scheme: EIP-4361 reads a bare domain as https.
        return std::format(
            "{} wants you to sign in with your Ethereum account:\n"
            "{}\n"
            "\n"
            "{}\n"
            "\n"
            "URI: {}\n"
            "Version: 1\n"
            "Chain ID: {}\n"
            "Nonce: {}\n"
            "Issued At: {}\n"
            "Expiration Time: {}",
            uri, chain::addressToChecksumHex(address), LOGIN_STATEMENT, uri,
            _login_settings.chain_id, nonce, utils::utcTimestamp(issued_at), utils::utcTimestamp(issued_at + LOGIN_TTL));
    }

    LoginChallenge AuthManager::issueLogin(const chain::Address & address, const std::string_view origin) const
    {
        const auto canonical = loginOrigin(origin);
        const std::size_t origin_index = canonical
            ? static_cast<std::size_t>(std::ranges::find(_login_settings.origins, *canonical) - _login_settings.origins.begin())
            : 0;
        const auto issued_at = std::chrono::floor<std::chrono::seconds>(_now());

        std::array<std::uint8_t, NONCE_SIZE> nonce_bytes{};
        crypto::writeUint64BE(nonce_bytes.data(), static_cast<std::uint64_t>(issued_at.time_since_epoch().count()));
        nonce_bytes[NONCE_TIME_SIZE] = static_cast<std::uint8_t>(origin_index);
        const std::string random = crypto::randomBytes(NONCE_RANDOM_SIZE);
        std::ranges::copy(random, nonce_bytes.begin() + NONCE_TIME_SIZE + 1);
        const auto mac = nonceMac(_nonce_key, address, std::span(nonce_bytes).first(NONCE_FIELDS_SIZE));
        std::ranges::copy(mac, nonce_bytes.begin() + NONCE_FIELDS_SIZE);

        LoginChallenge challenge{.nonce = evmc::hex(evmc::bytes_view(nonce_bytes.data(), nonce_bytes.size()))};
        challenge.message = loginMessage(address, origin_index, challenge.nonce, issued_at);
        return challenge;
    }

    std::optional<AuthManager::OpenedNonce> AuthManager::openNonce(const chain::Address & address, const std::string_view nonce) const
    {
        // Only the lower-case hex this server issues, so each nonce has a single spelling.
        if(nonce.size() != 2 * NONCE_SIZE
            || !std::ranges::all_of(nonce, [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        {
            return std::nullopt;
        }
        const auto bytes = evmc::from_hex(nonce);
        if(!bytes || bytes->size() != NONCE_SIZE) return std::nullopt;

        const std::span<const std::uint8_t> nonce_bytes(bytes->data(), bytes->size());
        const auto mac = nonceMac(_nonce_key, address, nonce_bytes.first(NONCE_FIELDS_SIZE));
        if(!crypto::equalConstantTime(asChars(mac), asChars(nonce_bytes.subspan(NONCE_FIELDS_SIZE))))
        {
            return std::nullopt;
        }

        const std::size_t origin_index = nonce_bytes[NONCE_TIME_SIZE];
        if(origin_index >= _login_settings.origins.size()) return std::nullopt;

        // 9999-12-31T23:59:59Z: the latest issue time whose message dates keep four-digit years.
        constexpr std::uint64_t MAX_ISSUED_SECONDS = 253402300799;
        const std::uint64_t issued_seconds = crypto::readUint64BE(nonce_bytes.data());
        if(issued_seconds > MAX_ISSUED_SECONDS) return std::nullopt;
        const std::chrono::sys_seconds issued_at{std::chrono::seconds{static_cast<std::int64_t>(issued_seconds)}};
        const auto expires_at = issued_at + LOGIN_TTL;
        if(expires_at <= _now()) return std::nullopt;

        return OpenedNonce{.message = loginMessage(address, origin_index, nonce, issued_at), .expires_at = expires_at};
    }

    asio::awaitable<std::expected<std::string, AuthError>> AuthManager::login(
        const chain::Address address, const std::string nonce, const std::string signature)
    {
        const auto opened = openNonce(address, nonce);
        if(!opened) co_return std::unexpected(AuthError{AuthError::Kind::INVALID_NONCE});
        if(!verifySignature(address, signature, opened->message)) co_return std::unexpected(AuthError{AuthError::Kind::INVALID_SIGNATURE});

        co_await async::ensureOnStrand(_strand);

        // Expired nonces fail openNonce, so only unexpired ones need remembering.
        // ponytail: per instance, so behind several instances a captured signature could be
        // replayed once on each within LOGIN_TTL; move this set to a shared store if that matters.
        const auto now = _now();
        while(!_redeemed_expiry.empty() && _redeemed_expiry.top().first <= now)
        {
            _redeemed.erase(_redeemed_expiry.top().second);
            _redeemed_expiry.pop();
        }
        if(!_redeemed.insert(nonce).second) co_return std::unexpected(AuthError{AuthError::Kind::INVALID_NONCE});
        _redeemed_expiry.emplace(opened->expires_at, nonce);

        co_return generateAccessToken(address);
    }

    bool AuthManager::verifySignature(const chain::Address & address, const std::string_view sig_hex, const std::string_view message)
    {
        const std::optional<evmc::bytes> signature_bytes_result = evmc::from_hex(sig_hex);
        if(!signature_bytes_result) return false;
        const auto & signature_bytes = *signature_bytes_result;

        if (signature_bytes.size() != 65) return false;

        uint8_t hash[crypto::Keccak256::HASH_LEN];
        std::string prefix = std::format("\x19" "Ethereum Signed Message:\n{}", message.size());
        prefix += message;
        dcn::crypto::Keccak256::getHash((const uint8_t*)prefix.data(), prefix.size(), hash);

        // v is 27/28 from personal_sign, or the bare recovery id. libsecp256k1 aborts the
        // process on a recovery id outside 0..3, so any other byte is refused here.
        int recid = signature_bytes[64];
        if (recid >= 27) recid -= 27;
        if (recid > 3) return false;

        // Parsing and recovery need neither signing tables nor randomization: the static context serves.
        secp256k1_ecdsa_recoverable_signature signature;
        if (!secp256k1_ecdsa_recoverable_signature_parse_compact(secp256k1_context_static, &signature, signature_bytes.data(), recid)) {
            return false;
        }

        secp256k1_pubkey pubkey;
        if (!secp256k1_ecdsa_recover(secp256k1_context_static, &pubkey, &signature, hash)) {
            return false;
        }

        uint8_t pubkey_serialized[65];
        size_t pubkey_len = 65;
        secp256k1_ec_pubkey_serialize(secp256k1_context_static, pubkey_serialized, &pubkey_len, &pubkey, SECP256K1_EC_UNCOMPRESSED);

        return address == chain::ethAddressFromPublicKey(pubkey_serialized, pubkey_len);
    }

    std::string AuthManager::generateAccessToken(const chain::Address & address) const
    {
        const auto now = _now();
        return jwt::create()
            .set_issuer(std::string(TOKEN_ISSUER))
            .set_type("JWS")
            .set_subject(evmc::hex(address))
            .set_issued_at(now)
            .set_expires_at(now + ACCESS_TOKEN_TTL)
            .sign(jwt::algorithm::hs256{_token_key});
    }

    std::expected<chain::Address, AuthError> AuthManager::verifyAccessToken(const std::string_view token) const
    {
        if(token.empty()) return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});

        try
        {
            const auto decoded = jwt::decode(std::string(token));
            jwt::verify<TokenClock, jwt::traits::kazuho_picojson>(TokenClock{&_now})
                .allow_algorithm(jwt::algorithm::hs256{_token_key})
                .with_issuer(std::string(TOKEN_ISSUER))
                .verify(decoded);

            const auto address = evmc::from_hex<chain::Address>(decoded.get_subject());
            if(!address) return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            return *address;
        }
        catch(const std::exception & e)
        {
            // A malformed, forged or expired token: the caller is refused, not the server failed.
            spdlog::debug("Access token verification failed: {}", e.what());
            return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
        }
    }
}
