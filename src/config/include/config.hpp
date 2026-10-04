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
    /// the server prepares and confirms them and holds no key for them. The deployer key
    /// is needed only to deploy or upgrade the PT contracts of the manifest deployment.
    struct PublishConfig
    {
        bool enabled = false;

        // Reviewed canonical manifest, or the one this server wrote when it deployed a
        // private deployment.
        std::filesystem::path manifest_path;

        // Deployment actions run with the deployer key at every startup that requests
        // them, registry first, before the server resolves its deployment. Each rewrites
        // the manifest. Deploying replaces the contract with a new proxy; upgrading points
        // the existing proxy at a new implementation. Deploying and upgrading the same
        // contract together is rejected.
        bool deploy_registry = false;
        bool deploy_runner = false;
        bool upgrade_registry = false;
        bool upgrade_runner = false;

        // The deployer key is accepted only from a protected file or an environment
        // secret; a command-line value would be visible in the process table.
        std::filesystem::path deployer_key_file;
        std::string deployer_key_env;
        std::uint64_t deployer_max_gas = 12'000'000;
        std::uint64_t deployer_max_fee_per_gas_wei = 200'000'000'000;

        bool hasDeploymentAction() const
        {
            return deploy_registry || deploy_runner || upgrade_registry || upgrade_runner;
        }
    };

    struct Config
    {
        std::filesystem::path bin_path;
        std::filesystem::path logs_path;
        std::filesystem::path resources_path;
        std::filesystem::path storage_path;

        bool verbose;

        std::uint32_t port;

        // Largest HTTP request body accepted, in bytes.
        std::uint32_t max_request_body_size;

        // Origins of the sites users sign in through; sign-in messages name only these.
        std::vector<std::string> auth_origins;

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
