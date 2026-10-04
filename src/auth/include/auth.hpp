#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
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

    /// How long an access token is accepted after it is issued.
    constexpr auto ACCESS_TOKEN_TTL = std::chrono::minutes{5};

    /// Fewest bytes a configured secret may have.
    constexpr std::size_t MIN_SECRET_SIZE = 32;

    /// Most sign-in origins a server can be configured with.
    constexpr std::size_t MAX_LOGIN_ORIGINS = 256;

    struct LoginSettings
    {
        /// Origins of the sites a user may sign in through, e.g. `https://decentralised.art`.
        /// The first is used when a request names none of them. Each is `http(s)://host[:port]`;
        /// AuthManager stores them as http::canonicalOrigin serializes them. A nonce names its
        /// origin by position, so instances sharing `secret` list the same origins in the same order.
        std::vector<std::string> origins{"http://localhost"};

        std::uint64_t chain_id = 1;

        /// Key for sign-in nonces and access tokens, at least MIN_SECRET_SIZE bytes. Instances
        /// that share it redeem each other's nonces and accept each other's tokens. Empty: a
        /// random key per instance, so nothing it issues outlives it or works on another.
        std::string secret{};
    };

    struct LoginChallenge
    {
        std::string nonce;

        /// The EIP-4361 sign-in message the wallet signs; /auth takes its nonce and the signature.
        std::string message{};
    };

    /**
     * @brief Issues and checks EIP-4361 sign-ins and the access tokens they grant.
     *
     * A nonce carries its issue time and origin under an HMAC, so issuing one stores nothing:
     * /nonce costs no memory however often it is called. The only state is the set of nonces
     * redeemed and not yet expired, which keeps each nonce to a single redemption.
     */
    class AuthManager
    {
        public:
            AuthManager() = delete;
            /// `now` is the time source for nonce and token expiry.
            /// @throws std::invalid_argument when `login_settings` has no origin, more than
            /// MAX_LOGIN_ORIGINS, one that is not an http(s)://host[:port] origin, or a secret
            /// shorter than MIN_SECRET_SIZE.
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
             */
            LoginChallenge issueLogin(const chain::Address & address, std::string_view origin) const;

            /**
             * @brief An access token for `address`, once `signature` is its wallet's signature of
             * the unexpired message issued to it under `nonce`.
             *
             * A nonce is redeemed at most once by this instance.
             * @return INVALID_NONCE for a nonce not issued to `address`, expired, or already
             * redeemed; INVALID_SIGNATURE for a signature of anything else.
             */
            asio::awaitable<std::expected<std::string, AuthError>> login(chain::Address address, std::string nonce, std::string signature);

            /// Whether `signature` is `address`'s EIP-191 `personal_sign` signature of `message`.
            static bool verifySignature(const chain::Address & address, std::string_view signature, std::string_view message);

            std::string generateAccessToken(const chain::Address & address) const;

            std::expected<chain::Address, AuthError> verifyAccessToken(std::string_view token) const;

        private:
            struct OpenedNonce
            {
                std::string message;
                std::chrono::system_clock::time_point expires_at;
            };

            std::string loginMessage(
                const chain::Address & address,
                std::size_t origin_index,
                std::string_view nonce,
                std::chrono::sys_seconds issued_at) const;

            /// The message `nonce` was issued for to `address`, if this key issued it and it is unexpired.
            std::optional<OpenedNonce> openNonce(const chain::Address & address, std::string_view nonce) const;

            asio::strand<asio::io_context::executor_type> _strand;

            const LoginSettings _login_settings;

            const std::function<std::chrono::system_clock::time_point()> _now;

            /// Keys derived from the secret, one per use.
            const std::string _nonce_key;
            const std::string _token_key;

            /// Redeemed nonces that have not expired yet.
            absl::flat_hash_set<std::string> _redeemed;

            /// (expiry, nonce) of `_redeemed`, soonest expiry on top.
            std::priority_queue<
                std::pair<std::chrono::system_clock::time_point, std::string>,
                std::vector<std::pair<std::chrono::system_clock::time_point, std::string>>,
                std::greater<>> _redeemed_expiry;
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

            case dcn::auth::AuthError::Kind::UNKNOWN : return formatter<string>::format("Unknown", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};
