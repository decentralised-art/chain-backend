#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <expected>
#include <format>
#include <functional>
#include <string_view>
#include <vector>
#include <regex>

#include <absl/container/flat_hash_map.h>
#include <spdlog/spdlog.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <jwt-cpp/jwt.h>

#include "async.hpp"
#include "keccak256.hpp"
#include "chain.hpp"

namespace dcn::auth
{
    struct AuthError
    {
        enum class Kind : std::uint8_t
        {
            UNKNOWN = 0,

            MISSING_TOKEN,
            INVALID_TOKEN,

            INVALID_SIGNATURE,

            INVALID_NONCE,

            INVALID_ADDRESS
        }
        kind = Kind::UNKNOWN;

        std::string message = "";
    };

    /// How long an issued sign-in message can be redeemed at /auth.
    constexpr auto LOGIN_TTL = std::chrono::minutes{5};

    /// Most sign-in messages awaiting /auth at once; bounds the memory /nonce can claim.
    constexpr std::size_t MAX_PENDING_LOGINS = 16384;

    /// Retained access tokens below which a login never sweeps expired ones.
    constexpr std::size_t ACCESS_TOKEN_SWEEP_MIN = 1024;

    struct LoginSettings
    {
        /// Origins of the sites a user may sign in through, e.g. `https://decentralised.art`.
        /// The first is used when a request names none of them. Each is `http(s)://host[:port]`;
        /// AuthManager stores them as http::canonicalOrigin serializes them.
        std::vector<std::string> origins{"http://localhost"};

        std::uint64_t chain_id = 1;
    };

    struct LoginChallenge
    {
        std::string nonce;

        /// The EIP-4361 sign-in message the wallet signs; /auth takes its nonce and the signature.
        std::string message;
    };

    class AuthManager
    {
        public:
            AuthManager() = delete;
            /// `now` is the time source for message and token expiry.
            /// @throws std::invalid_argument when `login_settings` has no origin or one that is
            /// not an http(s)://host[:port] origin.
            AuthManager(
                asio::io_context & io_context,
                LoginSettings login_settings = {},
                std::function<std::chrono::system_clock::time_point()> now = [] { return std::chrono::system_clock::now(); });

            AuthManager(const AuthManager&) = delete;
            AuthManager& operator=(const AuthManager&) = delete;

            ~AuthManager() = default;

            /// The configured sign-in origin `origin` names once canonicalized, if any.
            std::optional<std::string> loginOrigin(std::string_view origin) const;

            /**
             * @brief Issues a sign-in message for `address`.
             *
             * The message names `origin` when it is one of the configured origins, and the
             * first configured origin otherwise, so it never names a site that is not ours:
             * a wallet then warns a user signing it anywhere else.
             *
             * @return nullopt while MAX_PENDING_LOGINS unexpired messages await /auth.
             */
            asio::awaitable<std::optional<LoginChallenge>> issueLogin(const chain::Address & address, const std::string & origin);

            /**
             * @brief The unexpired sign-in message issued to `address` under `nonce`, for its
             * signature to be checked against.
             *
             * A known nonce is consumed either way, so a message can be redeemed once.
             * @return nullopt for an unknown nonce, an expired message, or another address.
             */
            asio::awaitable<std::optional<std::string>> consumeLogin(const chain::Address & address, const std::string & nonce);

            asio::awaitable<bool> verifySignature(const chain::Address & address, const std::string& signature, const std::string& message);

            asio::awaitable<std::string> generateAccessToken(const chain::Address & address);

            asio::awaitable<std::expected<chain::Address, AuthError>> verifyAccessToken(std::string token) const;

            asio::awaitable<void> invalidateAccessToken(const chain::Address & address);

            struct Storage
            {
                /// Access tokens held, expired ones not yet swept included.
                std::size_t access_tokens = 0;
                /// Slots the access token table has allocated.
                std::size_t access_token_capacity = 0;
                /// Sign-in messages awaiting /auth, expired ones not yet reclaimed included.
                std::size_t pending_logins = 0;
                /// Entries of the expiry queue, redeemed nonces not yet dropped included.
                std::size_t pending_login_queue = 0;
            };

            /// What the manager holds in memory.
            asio::awaitable<Storage> storage() const;

        private:
            struct AccessToken
            {
                std::string token;
                std::chrono::system_clock::time_point expires_at;
            };

            struct PendingLogin
            {
                chain::Address address;
                std::string message;
                std::chrono::system_clock::time_point expires_at;
            };

            asio::strand<asio::io_context::executor_type> _strand;

            /// HMAC key for access tokens, drawn per process: tokens do not outlive it anyway,
            /// since the issued tokens are only kept in memory.
            const std::string _SECRET;

            const LoginSettings _login_settings;

            const std::function<std::chrono::system_clock::time_point()> _now;

            static std::random_device _rng;

            /// Issued sign-in messages awaiting /auth, by nonce.
            absl::flat_hash_map<std::string, PendingLogin> _pending_logins;

            /// (expiry, nonce) of issued messages in issue order, which is expiry order.
            std::deque<std::pair<std::chrono::system_clock::time_point, std::string>> _pending_login_queue;

            absl::flat_hash_map<chain::Address, AccessToken> _access_tokens;

            /// Retained tokens at which the next login sweeps the expired ones.
            std::size_t _access_tokens_sweep_at = ACCESS_TOKEN_SWEEP_MIN;

            /// Time from which the next login sweeps the expired tokens whatever their number.
            std::chrono::system_clock::time_point _access_tokens_sweep_after{};
    };
}

template <>
struct std::formatter<dcn::auth::AuthError::Kind> : std::formatter<std::string> {
    auto format(const dcn::auth::AuthError::Kind & err, format_context& ctx) const {
        switch(err)
        {
            case dcn::auth::AuthError::Kind::MISSING_TOKEN : return formatter<string>::format("Missing token", ctx);
            case dcn::auth::AuthError::Kind::INVALID_TOKEN : return formatter<string>::format("Invalid token", ctx);
            case dcn::auth::AuthError::Kind::INVALID_SIGNATURE : return formatter<string>::format("Invalid signature", ctx);
            case dcn::auth::AuthError::Kind::INVALID_NONCE : return formatter<string>::format("Invalid nonce", ctx);
            case dcn::auth::AuthError::Kind::INVALID_ADDRESS : return formatter<string>::format("Invalid address", ctx);

            default:  return formatter<string>::format("Unknown", ctx);
        }
        return formatter<string>::format("", ctx);
    }
};