#include "json_rpc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>

#include <spdlog/spdlog.h>

#include "utils.hpp"

namespace dcn::rpc
{
    namespace
    {
        // EIP-1474 codes with a fixed meaning across providers.
        constexpr int JSON_RPC_INTERNAL_ERROR = -32603;
        constexpr int JSON_RPC_LIMIT_EXCEEDED = -32005;
        constexpr int HTTP_TOO_MANY_REQUESTS = 429;

        template<std::size_t N>
        bool _mentionsAny(const std::string & text, const std::array<std::string_view, N> & needles)
        {
            return std::ranges::any_of(needles, [&text](const std::string_view needle)
            {
                return text.find(needle) != std::string::npos;
            });
        }

        // The request asked for too much at once: a narrower range can still succeed.
        constexpr std::array<std::string_view, 9> SIZE_LIMIT_MESSAGES{
            "too many results",
            "query returned more than",
            "response size",
            "block range",
            "range too large",
            "range is too",
            "is limited to",
            "exceed maximum block range",
            "query timeout"};

        // The caller's plan allowance is used up until the provider's period resets.
        constexpr std::array<std::string_view, 5> QUOTA_MESSAGES{
            "monthly",
            "daily request count",
            "quota",
            "usage limit",
            "credit limit"};

        // The provider refuses the caller, not the request.
        constexpr std::array<std::string_view, 6> RATE_LIMIT_MESSAGES{
            "rate limit",
            "too many requests",
            "request rate",
            "compute units",
            "capacity",
            "throttl"};

        constexpr std::array<std::string_view, 3> ALREADY_KNOWN_MESSAGES{
            "already known",
            "known transaction",
            "already imported"};

        // The provider could not answer right now; the same request can succeed later.
        constexpr std::array<std::string_view, 8> TRANSIENT_MESSAGES{
            "header not found",
            "unknown block",
            "internal error",
            "temporarily",
            "try again",
            "service unavailable",
            "timed out",
            "timeout"};
    }

    RpcError classifyProviderError(const json & error)
    {
        RpcError out{.kind = RpcError::Kind::EXECUTION, .code = 0, .message = "provider error"};

        if(!error.is_object())
        {
            out.kind = RpcError::Kind::PROTOCOL;
            out.message = "JSON-RPC error is not an object";
            return out;
        }

        if(const auto code = error.find("code"); code != error.end() && code->is_number_integer())
        {
            out.code = static_cast<int>(code->get<std::int64_t>());
        }

        if(const auto message = error.find("message"); message != error.end() && message->is_string())
        {
            out.message = message->get<std::string>();
        }

        // Revert data arrives either as a hex string or nested one level deeper.
        if(const auto data = error.find("data"); data != error.end())
        {
            if(data->is_string())
            {
                out.data = data->get<std::string>();
            }
            else if(data->is_object())
            {
                if(const auto nested = data->find("data"); nested != data->end() && nested->is_string())
                {
                    out.data = nested->get<std::string>();
                }
            }
        }

        const std::string text = utils::toLower(out.message);

        if(_mentionsAny(text, SIZE_LIMIT_MESSAGES))
        {
            out.kind = RpcError::Kind::LIMIT;
        }
        else if(_mentionsAny(text, QUOTA_MESSAGES))
        {
            out.kind = RpcError::Kind::QUOTA;
        }
        else if(out.code == JSON_RPC_LIMIT_EXCEEDED || out.code == HTTP_TOO_MANY_REQUESTS
                || _mentionsAny(text, RATE_LIMIT_MESSAGES))
        {
            out.kind = RpcError::Kind::THROTTLED;
        }
        else if(out.code == JSON_RPC_INTERNAL_ERROR || _mentionsAny(text, TRANSIENT_MESSAGES))
        {
            out.kind = RpcError::Kind::TRANSPORT;
        }

        return out;
    }

    bool RpcError::retryable() const
    {
        return kind == Kind::TRANSPORT || kind == Kind::TIMEOUT || kind == Kind::THROTTLED;
    }

    RpcError RpcError::protocol(std::string message)
    {
        return RpcError{.kind = Kind::PROTOCOL, .code = 0, .message = std::move(message)};
    }

    bool mentions(const RpcError & error, const std::string_view needle)
    {
        return utils::toLower(error.message).find(needle) != std::string::npos;
    }

    bool mentionsQuotaExhausted(const std::string_view provider_text)
    {
        return _mentionsAny(utils::toLower(std::string(provider_text)), QUOTA_MESSAGES);
    }

    bool isAlreadyKnown(const RpcError & error)
    {
        const std::string text = utils::toLower(error.message);
        return _mentionsAny(text, ALREADY_KNOWN_MESSAGES);
    }

    std::string toString(const RpcError::Kind kind)
    {
        switch(kind)
        {
        case RpcError::Kind::TRANSPORT: return "transport";
        case RpcError::Kind::TIMEOUT:   return "timeout";
        case RpcError::Kind::THROTTLED: return "throttled";
        case RpcError::Kind::QUOTA:     return "quota";
        case RpcError::Kind::LIMIT:     return "limit";
        case RpcError::Kind::PROTOCOL:  return "protocol";
        case RpcError::Kind::EXECUTION: return "execution";
        }
        return "unknown";
    }

    JsonRpcClient::JsonRpcClient(std::shared_ptr<ITransport> transport, JsonRpcConfig config)
        : _transport(std::move(transport))
        , _config(std::move(config))
        , _jitter(static_cast<std::minstd_rand::result_type>(
              std::chrono::steady_clock::now().time_since_epoch().count()))
    {
    }

    std::string_view JsonRpcClient::endpoint() const
    {
        return _transport ? _transport->describe() : std::string_view{"<none>"};
    }

    void JsonRpcClient::cancel()
    {
        _cancelled = true;
        if(_transport) _transport->cancel();
    }

    std::uint64_t JsonRpcClient::_backoffMs(const std::size_t attempt)
    {
        const std::uint64_t exponential = _config.initial_backoff_ms << std::min<std::size_t>(attempt, 16);
        const std::uint64_t capped = std::min(exponential, _config.max_backoff_ms);
        // Full jitter: without it every ingestion loop retries in lockstep after a
        // provider hiccup and re-triggers the same throttle.
        return capped == 0 ? 0 : (_jitter() % (capped + 1));
    }

    asio::awaitable<std::expected<json, RpcError>> JsonRpcClient::_callOnce(
        const std::string_view method,
        const json & params)
    {
        const std::uint64_t id = _next_id++;

        const json request{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", std::string(method)},
            {"params", params}
        };

        auto body = co_await _transport->post(request.dump());
        if(!body)
        {
            co_return std::unexpected(body.error());
        }

        const json response = json::parse(*body, nullptr, false);
        if(response.is_discarded() || !response.is_object())
        {
            co_return std::unexpected(RpcError{
                .kind = RpcError::Kind::PROTOCOL, .code = 0, .message = "malformed JSON-RPC response"});
        }

        const auto response_id = response.find("id");
        if(response_id == response.end() || !response_id->is_number_unsigned()
           || response_id->get<std::uint64_t>() != id)
        {
            co_return std::unexpected(RpcError{
                .kind = RpcError::Kind::PROTOCOL, .code = 0, .message = "JSON-RPC id mismatch"});
        }

        if(const auto error = response.find("error"); error != response.end() && !error->is_null())
        {
            co_return std::unexpected(classifyProviderError(*error));
        }

        const auto result = response.find("result");
        if(result == response.end())
        {
            co_return std::unexpected(RpcError{
                .kind = RpcError::Kind::PROTOCOL, .code = 0, .message = "JSON-RPC response has no result"});
        }

        co_return *result;
    }

    asio::awaitable<std::expected<json, RpcError>> JsonRpcClient::call(
        const std::string_view method,
        json params,
        const bool retry_timeouts)
    {
        const RpcError cancelled{
            .kind = RpcError::Kind::TRANSPORT, .code = 0, .message = "JSON-RPC client is shut down"};

        if(!_transport)
        {
            co_return std::unexpected(RpcError{
                .kind = RpcError::Kind::PROTOCOL, .code = 0, .message = "no transport configured"});
        }

        const std::size_t attempts = std::max<std::size_t>(1, _config.max_attempts);

        RpcError last_error{};
        for(std::size_t attempt = 0; attempt < attempts; ++attempt)
        {
            if(_cancelled) co_return std::unexpected(cancelled);

            auto result = co_await _callOnce(method, params);
            if(result)
            {
                co_return result;
            }

            last_error = result.error();
            const bool retry = last_error.retryable()
                && (retry_timeouts || last_error.kind != RpcError::Kind::TIMEOUT);
            if(!retry || attempt + 1 == attempts || _cancelled)
            {
                break;
            }

            const std::uint64_t delay_ms = _backoffMs(attempt);
            spdlog::debug(
                "rpc {} on {} failed ({}), retrying in {}ms",
                method,
                _transport->describe(),
                toString(last_error.kind),
                delay_ms);

            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(delay_ms));
            std::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }

        co_return std::unexpected(last_error);
    }
}
