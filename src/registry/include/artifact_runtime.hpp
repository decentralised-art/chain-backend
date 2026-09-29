#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>

#include "artifact.hpp"
#include "http_transport.hpp"
#include "registry.hpp"

namespace dcn::registry
{
    // Makes the source of chain entities available: looks each registered artifact up in
    // the local stores, then in the HTTP gateways, verifies it against the entity's registry
    // event and records whether it is available, missing or invalid. Runs apart from event
    // ingestion, so an unavailable third-party artifact never stalls it.
    class ArtifactRuntime
    {
        public:
            ArtifactRuntime(
                asio::io_context & io_context,
                Registry & registry,
                const pt::ArtifactStore & artifacts,
                std::vector<std::shared_ptr<rpc::IHttpClient>> http_gateways = {},
                std::chrono::milliseconds interval = std::chrono::seconds(1));
            ~ArtifactRuntime();

            ArtifactRuntime(const ArtifactRuntime &) = delete;
            ArtifactRuntime & operator=(const ArtifactRuntime &) = delete;

            void start();
            void requestStop();
            asio::awaitable<void> stop();
            asio::awaitable<std::size_t> hydrateOnce(std::int64_t now_ms, std::size_t limit = 16);

        private:
            asio::awaitable<void> _run();
            asio::awaitable<pt::ArtifactRead> _fetchFromGateways(const std::string & content_hash);
            asio::awaitable<pt::ArtifactRead> _verify(const ArtifactReference & reference, pt::ArtifactRead artifact);

            asio::io_context & _io_context;
            Registry & _registry;
            const pt::ArtifactStore & _artifacts;
            std::vector<std::shared_ptr<rpc::IHttpClient>> _http_gateways;
            std::chrono::milliseconds _interval;
            asio::steady_timer _timer;
            asio::thread_pool _file_io{1};
            std::atomic<bool> _stop_requested{false};
            std::atomic<bool> _stopped{true};
    };
}
