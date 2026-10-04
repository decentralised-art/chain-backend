#include "auth.hpp"
#include "url.hpp"
#include "utils.hpp"

#include <algorithm>
#include <stdexcept>

namespace dcn::auth
{
    // initialize random number generator
    std::random_device AuthManager::_rng = std::random_device{};

    namespace
    {
        constexpr auto ACCESS_TOKEN_TTL = std::chrono::minutes{5};

        constexpr std::size_t LOGIN_NONCE_LENGTH = 16;
        constexpr std::string_view LOGIN_STATEMENT = "Sign in to decentralised.art.";

        std::string randomSecret(std::random_device & rng)
        {
            std::string secret(32, '\0');
            for(char & byte : secret) byte = static_cast<char>(rng());
            return secret;
        }

        // EIP-4361 requires at least 8 alphanumeric characters.
        std::string randomNonce(std::random_device & rng)
        {
            static constexpr std::string_view alphabet =
                "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
            std::string nonce(LOGIN_NONCE_LENGTH, '\0');
            for(char & c : nonce) c = alphabet[pick(rng)];
            return nonce;
        }
    }

    namespace
    {
        // Origins are stored as a browser serializes them, so they compare equal to the
        // `window.location.origin` or `Origin` a page sends.
        LoginSettings canonicalSettings(LoginSettings settings)
        {
            if(settings.origins.empty()) throw std::invalid_argument("At least one sign-in origin is required");
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
            return settings;
        }
    }

    AuthManager::AuthManager(
        asio::io_context & io_context,
        LoginSettings login_settings,
        std::function<std::chrono::system_clock::time_point()> now)
    :   _strand(asio::make_strand(io_context)),
        _SECRET{randomSecret(_rng)},
        _login_settings(canonicalSettings(std::move(login_settings))),
        _now(std::move(now))
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

    asio::awaitable<std::optional<LoginChallenge>> AuthManager::issueLogin(const chain::Address & address, const std::string & origin)
    {
        co_await async::ensureOnStrand(_strand);

        // LOGIN_TTL is fixed, so messages expire in the order they were issued and expired
        // ones are reclaimed from the front of the queue: amortized O(1) per request. A queued
        // nonce that was already redeemed is skipped.
        // ponytail: a system clock stepped back delays reclaiming by the step; consumeLogin
        // still checks each expiry, so only capacity is affected.
        const auto now = _now();
        while(!_pending_login_queue.empty() && _pending_login_queue.front().first <= now)
        {
            const auto & [expires_at, nonce] = _pending_login_queue.front();
            if(const auto it = _pending_logins.find(nonce); it != _pending_logins.end() && it->second.expires_at == expires_at)
            {
                _pending_logins.erase(it);
            }
            _pending_login_queue.pop_front();
        }
        if(_pending_logins.size() >= MAX_PENDING_LOGINS) co_return std::nullopt;

        const std::string uri = loginOrigin(origin).value_or(_login_settings.origins.front());
        const auto expires_at = now + LOGIN_TTL;

        LoginChallenge challenge{.nonce = randomNonce(_rng)};
        // The first line keeps the origin's scheme: EIP-4361 reads a bare domain as https.
        challenge.message = std::format(
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
            _login_settings.chain_id, challenge.nonce, utils::utcTimestamp(now), utils::utcTimestamp(expires_at));

        _pending_logins[challenge.nonce] = PendingLogin{.address = address, .message = challenge.message, .expires_at = expires_at};
        _pending_login_queue.emplace_back(expires_at, challenge.nonce);

        // Redeemed nonces stay queued until they expire. Dropping them whenever the queue
        // passes twice the cap leaves only pending ones, so the queue stays within twice the
        // cap at amortized O(1) per request.
        if(_pending_login_queue.size() > 2 * MAX_PENDING_LOGINS)
        {
            std::erase_if(_pending_login_queue, [this](const auto & entry)
            {
                const auto it = _pending_logins.find(entry.second);
                return it == _pending_logins.end() || it->second.expires_at != entry.first;
            });
        }
        co_return challenge;
    }

    asio::awaitable<std::optional<std::string>> AuthManager::consumeLogin(const chain::Address & address, const std::string & nonce)
    {
        co_await async::ensureOnStrand(_strand);

        const auto it = _pending_logins.find(nonce);
        if(it == _pending_logins.end()) co_return std::nullopt;

        PendingLogin pending = std::move(it->second);
        _pending_logins.erase(it);
        if(pending.address != address || pending.expires_at <= _now()) co_return std::nullopt;
        co_return std::move(pending.message);
    }

    asio::awaitable<bool> AuthManager::verifySignature(const chain::Address & address, const std::string& sig_hex, const std::string& message)
    {
        co_await async::ensureOnStrand(_strand);
        
        // verify signature
        const std::optional<evmc::bytes> signature_bytes_result = evmc::from_hex(sig_hex);
        if(!signature_bytes_result) co_return false;
        const auto & signature_bytes = *signature_bytes_result;

        if (signature_bytes.size() != 65) co_return false;

        uint8_t hash[crypto::Keccak256::HASH_LEN];
        std::string prefix(1, '\x19');
        prefix += "Ethereum Signed Message:\n" + std::to_string(message.size()) + message;
        dcn::crypto::Keccak256::getHash((const uint8_t*)prefix.data(), prefix.size(), hash);

        // v is 27/28 from personal_sign, or the bare recovery id. libsecp256k1 aborts the
        // process on a recovery id outside 0..3, so any other byte is refused here.
        int recid = signature_bytes[64];
        if (recid >= 27) recid -= 27;
        if (recid > 3) co_return false;

        secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
        secp256k1_ecdsa_recoverable_signature signature;

        if (!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &signature, signature_bytes.data(), recid)) {
            secp256k1_context_destroy(ctx);
            co_return false;
        }

        secp256k1_pubkey pubkey;
        if (!secp256k1_ecdsa_recover(ctx, &pubkey, &signature, hash)) {
            secp256k1_context_destroy(ctx);
            co_return false;
        }

        uint8_t pubkey_serialized[65];
        size_t pubkey_len = 65;
        secp256k1_ec_pubkey_serialize(ctx, pubkey_serialized, &pubkey_len, &pubkey, SECP256K1_EC_UNCOMPRESSED);
        secp256k1_context_destroy(ctx);

        co_return address == chain::ethAddressFromPublicKey(pubkey_serialized, pubkey_len);
    }

    asio::awaitable<std::string> AuthManager::generateAccessToken(const chain::Address & address)
    {
        co_await async::ensureOnStrand(_strand);

        const auto now = _now();
        const auto expires_at = now + ACCESS_TOKEN_TTL;

        // A login sweeps once the map has doubled since the last sweep, or a token TTL after
        // it: O(1) amortized per login, and after any login the map holds no token that
        // expired before the previous sweep. Erasing keeps the table's capacity, so a sweep
        // that leaves it mostly empty also shrinks it: storage and later sweeps then follow
        // recent logins, not an old peak.
        if(_access_tokens.size() >= _access_tokens_sweep_at || now >= _access_tokens_sweep_after)
        {
            absl::erase_if(_access_tokens, [now](const auto & entry) { return entry.second.expires_at <= now; });
            if(_access_tokens.capacity() > 4 * std::max(_access_tokens.size(), ACCESS_TOKEN_SWEEP_MIN))
            {
                _access_tokens.rehash(0);
            }
            _access_tokens_sweep_at = std::max(ACCESS_TOKEN_SWEEP_MIN, 2 * _access_tokens.size());
            _access_tokens_sweep_after = now + ACCESS_TOKEN_TTL;
        }

        auto token = jwt::create()
            .set_issuer("eth-auth-demo")
            .set_type("JWS")
            .set_subject(evmc::hex(address))
            .set_issued_at(now)
            .set_expires_at(expires_at)
            .sign(jwt::algorithm::hs256{_SECRET});

        _access_tokens[address] = AccessToken{.token = token, .expires_at = expires_at};

        co_return token;
    }

    asio::awaitable<std::expected<chain::Address, AuthError>> AuthManager::verifyAccessToken(std::string token) const
    {
        co_await async::ensureOnStrand(_strand);

        if(token.empty()) co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
        if(token.back() == '\r')token.pop_back(); // remove \r if present

        try 
        {
            auto decoded = jwt::decode(token);

            auto verifier = jwt::verify()
                .allow_algorithm(jwt::algorithm::hs256{_SECRET})
                .with_issuer("eth-auth-demo");

            verifier.verify(decoded);

            auto address_result = evmc::from_hex<chain::Address>(decoded.get_subject());
            if(!address_result)
            {
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }
            const auto &  address = *address_result;

            if(_access_tokens.contains(address) == false)
            {
                co_return std::unexpected(AuthError{AuthError::Kind::MISSING_TOKEN});
            }

            // The retained record decides expiry, on the same time source that issued it.
            const AccessToken & retained = _access_tokens.at(address);
            if(token != retained.token || retained.expires_at <= _now())
            {
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }

            co_return address;

        } 
        catch(const jwt::error::token_verification_exception & e)
        {
            if(e.code() == jwt::error::token_verification_error::token_expired)
            {
                spdlog::error("Access token verification failed. Token expired");
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }

            if(e.code() == jwt::error::token_verification_error::wrong_algorithm)
            {
                spdlog::error("Access token verification failed. Wrong algorithm");
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }

            if(e.code() == jwt::error::token_verification_error::audience_missmatch)
            {
                spdlog::error("Access token verification failed. Audience mismatch");
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }

            if(e.code() == jwt::error::token_verification_error::claim_type_missmatch)
            {
                spdlog::error("Access token verification failed. Claim type mismatch");
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }

            if(e.code() == jwt::error::token_verification_error::claim_value_missmatch)
            {
                spdlog::error("Access token verification failed. Claim value mismatch");
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }

            if(e.code() == jwt::error::token_verification_error::missing_claim)
            {
                spdlog::error("Access token verification failed. Missing claim");
                co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
            }
        }
        catch(const std::exception & e)
        {
            // A malformed token or a bad signature: the caller is refused, not the server failed.
            spdlog::error("Access token verification failed: {}", e.what());
            co_return std::unexpected(AuthError{AuthError::Kind::INVALID_TOKEN});
        }

        spdlog::error("Access token verification failed");
        co_return std::unexpected(AuthError{AuthError::Kind::UNKNOWN});
    }

    asio::awaitable<void> AuthManager::invalidateAccessToken(const chain::Address & address)
    {
        co_await async::ensureOnStrand(_strand);

        _access_tokens.erase(address);
    }

    asio::awaitable<AuthManager::Storage> AuthManager::storage() const
    {
        co_await async::ensureOnStrand(_strand);

        co_return Storage{
            .access_tokens = _access_tokens.size(),
            .access_token_capacity = _access_tokens.capacity(),
            .pending_logins = _pending_logins.size(),
            .pending_login_queue = _pending_login_queue.size()};
    }

}