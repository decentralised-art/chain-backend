#include "artifact_runtime.hpp"

#include <algorithm>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "utils.hpp"

namespace dcn::registry
{
    ArtifactRuntime::ArtifactRuntime(
        asio::io_context & io_context,
        Registry & registry,
        const pt::ArtifactStore & artifacts,
        std::vector<std::shared_ptr<rpc::IHttpClient>> http_gateways,
        const std::chrono::milliseconds interval)
        : _io_context(io_context)
        , _registry(registry)
        , _artifacts(artifacts)
        , _http_gateways(std::move(http_gateways))
        , _interval(interval)
        , _timer(io_context)
    {}

    ArtifactRuntime::~ArtifactRuntime()
    {
        requestStop();
        _file_io.stop();
        _file_io.join();
    }

    void ArtifactRuntime::start()
    {
        if(!_stopped.exchange(false)) return;
        _stop_requested.store(false);
        asio::co_spawn(_io_context, _run(), [this](std::exception_ptr error)
        {
            _stopped.store(true);
            if(error) utils::logException(error, "Artifact runtime failed");
        });
    }

    void ArtifactRuntime::requestStop()
    {
        _stop_requested.store(true);
        _timer.cancel();
        for(const auto & gateway : _http_gateways)
        {
            gateway->cancel();
        }
    }

    asio::awaitable<void> ArtifactRuntime::stop()
    {
        requestStop();
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer wait(executor);
        while(!_stopped.load())
        {
            wait.expires_after(std::chrono::milliseconds(10));
            std::error_code ec;
            co_await wait.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if(ec && ec != asio::error::operation_aborted) break;
        }
    }

    asio::awaitable<pt::ArtifactRead> ArtifactRuntime::_fetchFromGateways(const std::string & content_hash)
    {
        bool invalid_seen = false;
        for(const auto & gateway : _http_gateways)
        {
            std::string target = gateway->endpoint().target;
            if(!target.ends_with('/')) target.push_back('/');
            target += content_hash + ".json";

            const auto response = co_await gateway->request("GET", std::move(target), "");
            if(!response || response->status != 200)
            {
                continue;
            }

            // Only bytes that hash to the requested address are cached; anything else is
            // a broken or hostile gateway and the next one is asked.
            const std::string bytes = response->body;
            const auto cached = co_await asio::co_spawn(
                _file_io,
                [this, bytes]() -> asio::awaitable<std::expected<std::string, std::string>>
                {
                    co_return _artifacts.cache(bytes);
                },
                asio::use_awaitable);
            if(!cached || *cached != content_hash)
            {
                invalid_seen = true;
                spdlog::warn(
                    "Artifact gateway {} served bytes that do not verify as {}",
                    gateway->endpoint().redacted(),
                    content_hash);
                continue;
            }

            co_return pt::ArtifactRead{.state = pt::ArtifactState::AVAILABLE, .bytes = bytes};
        }

        co_return pt::ArtifactRead{
            .state = invalid_seen ? pt::ArtifactState::INVALID : pt::ArtifactState::MISSING,
            .error = invalid_seen ? "gateways served unverifiable bytes" : "artifact is unavailable"};
    }

    asio::awaitable<pt::ArtifactRead> ArtifactRuntime::_verify(
        const ArtifactReference & reference,
        pt::ArtifactRead artifact)
    {
        const auto bundle = pt::decodeArtifactBundle(*artifact.bytes, reference.content_hash);
        if(!bundle || bundle->kind != reference.kind || bundle->name != reference.name
           || bundle->metadata_hash != reference.metadata_hash
           || bundle->runtime_code_hash != reference.runtime_code_hash)
        {
            co_return pt::ArtifactRead{
                .state = pt::ArtifactState::INVALID,
                .error = "artifact identity does not match registry event"};
        }

        if(reference.kind != EntityKind::CONNECTOR)
        {
            co_return artifact;
        }

        // The registry record of a chain connector comes from the arrays of its event; they
        // must describe the connector the artifact actually built.
        const auto location = co_await _registry.getEntityLocation(EntityKind::CONNECTOR, reference.name);
        const auto projected = co_await _registry.getConnectorRecordHandle(reference.name);
        if(!location || location->source.source_id != reference.source_id || !projected || !*projected)
        {
            co_return artifact;
        }

        const auto built = parse::parseFromJson<ConnectorRecord>(bundle->definition.dump(), parse::use_protobuf);
        const auto from_artifact = built ? pt::buildConnectorRegistration(built->connector()) : std::nullopt;
        const auto from_event = pt::buildConnectorRegistration((**projected).connector());
        if(!from_artifact || !from_event || *from_artifact != *from_event)
        {
            co_return pt::ArtifactRead{
                .state = pt::ArtifactState::INVALID,
                .error = "registry event does not describe the connector its artifact built"};
        }

        co_return artifact;
    }

    asio::awaitable<std::size_t> ArtifactRuntime::hydrateOnce(
        const std::int64_t now_ms,
        const std::size_t limit)
    {
        const auto references = co_await _registry.getArtifactsDue(now_ms, limit);
        std::size_t available = 0;
        for(const auto & reference : references)
        {
            const std::string content_hash = reference.content_hash;
            pt::ArtifactRead artifact = co_await asio::co_spawn(
                _file_io,
                [this, content_hash]() -> asio::awaitable<pt::ArtifactRead>
                {
                    co_return _artifacts.read(content_hash);
                },
                asio::use_awaitable);

            if(artifact.state == pt::ArtifactState::MISSING && !_http_gateways.empty())
            {
                artifact = co_await _fetchFromGateways(content_hash);
            }

            if(artifact.bytes)
            {
                artifact = co_await _verify(reference, std::move(artifact));
            }

            const std::uint32_t exponent = std::min<std::uint32_t>(reference.attempts, 8);
            const std::int64_t retry_at = artifact.state == pt::ArtifactState::AVAILABLE
                ? 0
                : now_ms + std::min<std::int64_t>(300'000, 1'000LL << exponent);
            if(!co_await _registry.setArtifactState(
                reference.kind,
                reference.source_id,
                reference.name,
                artifact.state,
                retry_at))
            {
                spdlog::warn(
                    "Artifact state update failed for source='{}' name='{}'",
                    reference.source_id,
                    reference.name);
                continue;
            }
            if(artifact.state == pt::ArtifactState::AVAILABLE) ++available;
        }
        co_return available;
    }

    asio::awaitable<void> ArtifactRuntime::_run()
    {
        while(!_stop_requested.load())
        {
            co_await hydrateOnce(utils::nowMs());
            _timer.expires_after(_interval);
            std::error_code ec;
            co_await _timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if(ec && ec != asio::error::operation_aborted)
            {
                spdlog::warn("Artifact runtime timer failed: {}", ec.message());
            }
        }
    }
}
