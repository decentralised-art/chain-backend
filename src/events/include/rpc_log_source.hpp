#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio.hpp>

#include "eth_client.hpp"

#include "emitted_log_source.hpp"

namespace dcn::events
{
    // topic0 values of every registry event the server can decode, plus the ERC-1967
    // Upgraded(address) event so a proxy upgrade is observed rather than missed.
    const std::vector<std::string> & ptRegistryTopics();

    struct RpcLogSourceConfig
    {
        // Stable logical identity of this source. Excludes the provider URL so that
        // switching providers resumes the same cursor instead of forking the chain.
        std::string source_id{};
        std::string chain_namespace{};

        std::uint64_t chain_id = 1;
        std::string registry_address{};

        // First block that can contain registry events.
        std::int64_t deployment_block = 0;

        std::int64_t max_batch_blocks = 500;

        // Used only while the endpoint does not serve the safe/finalized block tags.
        unsigned int confirmations = 12;

        // How long the safe/finalized block tags are reused before they are read again.
        // They move once per epoch (6.4 minutes on Ethereum), and a stale value only
        // delays finality, never overstates it.
        std::chrono::milliseconds finality_refresh_interval{300'000};

        // Code hash the registry implementation must have after a proxy upgrade. Empty
        // accepts any implementation that reports the supported protocol version.
        std::string expected_implementation_code_hash{};

        std::chrono::milliseconds metrics_log_interval{60'000};
    };

    struct RpcLogSourceMetrics
    {
        std::uint64_t polls = 0;
        std::uint64_t failed_polls = 0;
        std::uint64_t range_requests = 0;
        std::uint64_t provider_limit_responses = 0;
        std::uint64_t scanned_blocks = 0;
        std::uint64_t logs_received = 0;
        std::uint64_t unparsable_logs = 0;
        std::int64_t last_head = 0;
        std::int64_t lag_blocks = 0;
        std::int64_t applied_finalized = -1;
    };

    // Reads PT registry logs from any standard Ethereum JSON-RPC endpoint.
    //
    // Every poll scans one bounded block range and reports it so the runtime can
    // reconcile it. The ranges form cycles: each cycle starts right after the finality
    // already applied, re-verifies the non-final tail, and continues to the head, so the
    // tail is reconciled on every cycle and after every restart. Finality is only
    // reported for blocks a cycle scanned while the provider already considered them
    // final, so an unreconciled row is never finalized. The cursor never moves back.
    class RpcLogSource final : public IEmittedLogSource
    {
        public:
            RpcLogSource(std::shared_ptr<rpc::EthClient> client, RpcLogSourceConfig config);

            std::string_view sourceId() const override;
            std::uint64_t chainId() const override;
            std::string_view chainNamespace() const override;
            CursorKind cursorKind() const override;

            void restoreFinality(const FinalityCheckpoint & checkpoint) override;

            asio::awaitable<SourcePoll> pollSince(std::uint64_t cursor, std::size_t limit) override;
            void committed() override;

            void cancel() override;

            bool paused() const;
            RpcLogSourceMetrics metrics() const;

        private:
            struct Heights
            {
                rpc::BlockHeader head{};
                std::int64_t safe = 0;
                std::int64_t finalized = 0;
                std::string finalized_hash{};   // empty when the finalized height came from confirmations
            };

            asio::awaitable<std::optional<Heights>> _readHeights();
            // nullopt when the anchor block could not be read.
            asio::awaitable<std::optional<bool>> _anchorHolds(const Heights & heights);
            asio::awaitable<std::optional<std::vector<rpc::json>>> _getLogsNarrowing(
                std::int64_t from_block,
                std::int64_t to_block,
                std::int64_t & scanned_to);
            asio::awaitable<bool> _isSupportedImplementation();
            void _logMetrics();

            std::shared_ptr<rpc::EthClient> _client;
            RpcLogSourceConfig _config;
            std::string _upgraded_topic;

            std::int64_t _batch_blocks = 1;

            // Current reconciliation cycle: the next block to scan (none between cycles)
            // and the highest block up to which every block since the cycle start was
            // scanned while final.
            struct Cycle
            {
                std::optional<std::int64_t> next;
                std::int64_t final_through = -1;
                std::int64_t applied_finalized = -1;
                std::string anchor_hash;
            };
            Cycle _cycle;

            // State the last poll would move to; it replaces _cycle once the runtime
            // reports the poll committed.
            std::optional<Cycle> _pending;

            bool _finality_tags_supported = true;
            std::chrono::steady_clock::time_point _finality_tags_retry_at{};

            struct FinalityTags
            {
                rpc::BlockHeader safe;
                rpc::BlockHeader finalized;
            };
            std::optional<FinalityTags> _finality_tags;
            std::chrono::steady_clock::time_point _finality_tags_read_at{};

            bool _paused = false;
            RpcLogSourceMetrics _metrics;
            std::chrono::steady_clock::time_point _metrics_logged_at{};
    };
}
