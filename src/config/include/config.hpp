#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dcn::config
{
    struct IngestionConfig
    {
        bool enabled = false;
        bool use_local_evm_source = false;

        // Provider URLs commonly embed an API key, so the URL itself is never logged.
        std::string rpc_url;
        std::string registry_address;
        std::string runner_address;
        std::optional<std::uint64_t> start_block = std::nullopt;

        unsigned int poll_interval_ms;
        unsigned int confirmations;
        unsigned int block_batch_size;
    };

    /// POST /execute of a chain connector: an eth_call of Runner.gen.
    struct ExecuteConfig
    {
        std::uint64_t max_gas = 100'000'000;
        std::string block_tag = "safe";
    };

    /// On-chain publication. Owners send and pay for their own publication transactions:
    /// the server prepares and confirms them and holds no key for them. A key is needed
    /// only to bootstrap a private deployment.
    struct PublishConfig
    {
        bool enabled = false;

        // Reviewed canonical manifest, or the one this server wrote when it bootstrapped
        // a private deployment.
        std::filesystem::path manifest_path;

        // Deploy registry/runner when no deployment is configured. Suitable for private
        // and development chains; a canonical public deployment is an explicit release
        // operation with a committed manifest.
        bool bootstrap = false;

        // The bootstrap key is accepted only from a protected file or an environment
        // secret; a command-line value would be visible in the process table.
        std::filesystem::path bootstrap_key_file;
        std::string bootstrap_key_env = "DCN_BOOTSTRAP_KEY";
        std::uint64_t bootstrap_max_gas = 12'000'000;
        std::uint64_t bootstrap_max_fee_per_gas_wei = 200'000'000'000;
    };

    struct Config
    {
        std::filesystem::path bin_path;
        std::filesystem::path logs_path;
        std::filesystem::path resources_path;
        std::filesystem::path storage_path;

        // http(s) URLs and local directories that serve artifacts by content hash.
        std::vector<std::string> artifact_gateways;
        std::vector<std::filesystem::path> artifact_replicas;

        bool verbose;

        std::uint32_t port;

        IngestionConfig chain_ingestion;
        ExecuteConfig execute;
        PublishConfig publish;

        unsigned int registry_wal_sync_ms;
        std::filesystem::path registry_db;

        std::filesystem::path events_db;

        std::filesystem::path feed_db;
        std::filesystem::path feed_archive_root;
        std::uint64_t events_chain_id = 1;
        unsigned int events_hot_window_days = 90;
        unsigned int events_projector_interval_ms = 200;
        unsigned int events_archive_interval_ms = 30000;
        unsigned int events_reorg_window_blocks = 2048;
        unsigned int events_outbox_retention_days = 7;
        unsigned int events_projector_retry_attempts = 5;
        unsigned int events_dead_letter_sweep_ms = 60000;
    };
}
