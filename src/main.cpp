#include "decentralised_art.hpp"
#include "feed_projector.hpp"
#include "feed_runtime.hpp"
#include "registry_projector.hpp"
#include "artifact_runtime.hpp"
#include "rpc.hpp"
#include <array>
#include <charconv>
#include <exception>
#include <string_view>

static std::vector<std::string> _splitList(const std::string & value)
{
    std::vector<std::string> items;
    std::size_t start = 0;
    while(start <= value.size())
    {
        const std::size_t end = value.find(';', start);
        std::string item = value.substr(start, end - start);
        if(!item.empty()) items.push_back(std::move(item));
        if(end == std::string::npos) break;
        start = end + 1;
    }
    return items;
}

static std::optional<std::uint64_t> _parseUint64(const std::string & value)
{
    std::uint64_t parsed = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if(ec != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return parsed;
}

static std::optional<std::string> _parseAddress(const std::string & value)
{
    const auto address = evmc::from_hex<dcn::chain::Address>(value);
    if(!address) return std::nullopt;
    return dcn::chain::addressToHex(*address);
}

#ifndef Solidity_SOLC_EXECUTABLE
    #error "Solidity_SOLC_EXECUTABLE is not defined"
#endif

static bool _createDirectories(const dcn::config::Config & cfg)
{
    std::error_code creation_dir_ec;

    // Create storage directory if it doesn't exist
    if(std::filesystem::exists(cfg.storage_path) == false)
    {
        std::filesystem::create_directory(cfg.storage_path, creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create storage directory '{}': {}",
                cfg.storage_path.string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create connectors directory if it doesn't exist
    if(std::filesystem::exists(cfg.storage_path / "connectors") == false)
    {
        std::filesystem::create_directory(cfg.storage_path / "connectors", creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create connectors directory '{}': {}",
                (cfg.storage_path / "connectors").string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create connectors build directory if it doesn't exist
    if(std::filesystem::exists(cfg.storage_path / "connectors" / "build") == false)
    {
        std::filesystem::create_directory(cfg.storage_path / "connectors" / "build", creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create connectors build directory '{}': {}",
                (cfg.storage_path / "connectors" / "build").string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create transformations directory if it doesn't exist
    if(std::filesystem::exists(cfg.storage_path / "transformations") == false)
    {
        std::filesystem::create_directory(cfg.storage_path / "transformations", creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create transformations directory '{}': {}",
                (cfg.storage_path / "transformations").string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create transformations build directory if it doesn't exist
    if(std::filesystem::exists(cfg.storage_path / "transformations" / "build") == false)
    {
        std::filesystem::create_directory(cfg.storage_path / "transformations" / "build", creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create transformations build directory '{}': {}",
                (cfg.storage_path / "transformations" / "build").string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create conditions directory if it doesn't exist
    if(std::filesystem::exists(cfg.storage_path / "conditions") == false)
    {
        std::filesystem::create_directory(cfg.storage_path / "conditions", creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create conditions directory '{}': {}",
                (cfg.storage_path / "conditions").string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create conditions build directory if it doesn't exist
    if(std::filesystem::exists(cfg.storage_path / "conditions" / "build") == false)
    {
        std::filesystem::create_directory(cfg.storage_path / "conditions" / "build", creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create conditions build directory '{}': {}",
                (cfg.storage_path / "conditions" / "build").string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create registry DB directory if it doesn't exist
    if(!cfg.registry_db.parent_path().empty() && std::filesystem::exists(cfg.registry_db.parent_path()) == false)
    {
        std::filesystem::create_directories(cfg.registry_db.parent_path(), creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create registry DB directory '{}': {}",
                cfg.registry_db.parent_path().string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create events DB directory if it doesn't exist
    if(!cfg.events_db.parent_path().empty() && std::filesystem::exists(cfg.events_db.parent_path()) == false)
    {
        std::filesystem::create_directories(cfg.events_db.parent_path(), creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create events DB directory '{}': {}",
                cfg.events_db.parent_path().string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create feed DB directory if it doesn't exist
    if(!cfg.feed_db.parent_path().empty() && std::filesystem::exists(cfg.feed_db.parent_path()) == false)
    {
        std::filesystem::create_directories(cfg.feed_db.parent_path(), creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create feed DB directory '{}': {}",
                cfg.feed_db.parent_path().string(),
                creation_dir_ec.message());
            return false;
        }
    }

    // Create feed archive directory if it doesn't exist
    if(!cfg.feed_archive_root.empty() && std::filesystem::exists(cfg.feed_archive_root) == false)
    {
        std::filesystem::create_directories(cfg.feed_archive_root, creation_dir_ec);
        if(creation_dir_ec)
        {
            spdlog::error(
                "Failed to create feed archive directory '{}': {}",
                cfg.feed_archive_root.string(),
                creation_dir_ec.message());
            return false;
        }
    }

    return true;
}

static void _configureLogger(const std::filesystem::path& logs_path)
{
    std::filesystem::create_directories(logs_path);

    // Create sinks
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    const std::string log_name = dcn::utils::currentTimestamp() + "-DCNServer.log";
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        (logs_path / log_name).string(), true);

    // set different log levels per sink
    console_sink->set_level(spdlog::level::info);
    file_sink->set_level(spdlog::level::debug);
    console_sink->set_pattern("[%T] [%^%l%$] %v");
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
    
    spdlog::logger logger("multi_sink", {console_sink, file_sink});
    logger.set_level(spdlog::level::debug);
    logger.flush_on(spdlog::level::info);

    spdlog::set_default_logger(std::make_shared<spdlog::logger>(logger));
}

static std::vector<int> _configureShutdownSignals()
{
    std::vector<int> shutdown_signal_ids{
        SIGINT,
        SIGTERM
    };
#if defined(SIGBREAK)
    shutdown_signal_ids.push_back(SIGBREAK);
#endif
#if defined(SIGQUIT)
    shutdown_signal_ids.push_back(SIGQUIT);
#endif
#if defined(SIGHUP)
    shutdown_signal_ids.push_back(SIGHUP);
#endif
    return shutdown_signal_ids;
}

static asio::awaitable<void> _runStartupAndListen(
    dcn::registry::Registry & registry,
    dcn::evm::EVM & evm,
    dcn::server::Server & server,
    const dcn::config::Config & cfg)
{
    spdlog::info("Starting JSON storage import...");

    const bool import_success = co_await dcn::loader::importJsonStorageToDatabase(
        evm,
        registry,
        cfg.storage_path);

    if(!import_success)
    {
        spdlog::warn("JSON storage import finished with errors");
    }
    else
    {
        spdlog::info("JSON storage import finished");
    }

    co_await server.listen();
    co_return;
}

static asio::awaitable<void> _runGracefulShutdown(
    dcn::server::Server & server,
    std::optional<dcn::storage::sqlite::WalSyncWorker> & wal_sync_worker,
    std::atomic<bool> & wal_sync_worker_stopped,
    dcn::registry::Registry & registry,
    bool wal_enabled,
    dcn::events::EventRuntime & events_runtime,
    dcn::feed::FeedRuntime & feed_runtime,
    dcn::registry::ArtifactRuntime & artifact_runtime)
{
    spdlog::info("Decentralised Art server stopping...");
    // Stop events first: its projectors write into feed_runtime.feed(), so the feed
    // must outlive them. Stopping feed first would let a final projector pass write
    // after the feed's final checkpoint, losing those rows.
    co_await events_runtime.stop();
    spdlog::info("Events runtime stopped");
    co_await artifact_runtime.stop();
    spdlog::info("Artifact runtime stopped");
    co_await feed_runtime.stop();
    spdlog::info("Feed runtime stopped");
    co_await server.close();
    spdlog::info("Decentralised Art server close requested");

    if(wal_sync_worker != std::nullopt)
    {
        spdlog::info("Requesting registry WAL sync worker stop...");
        wal_sync_worker->requestStop();

        auto exec = co_await asio::this_coro::executor;
        asio::steady_timer wait_timer(exec);

        while(!wal_sync_worker_stopped.load(std::memory_order_acquire))
        {
            wait_timer.expires_after(std::chrono::milliseconds(25));
            std::error_code wait_ec;
            co_await wait_timer.async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));
            if(wait_ec)
            {
                if(wait_ec != asio::error::operation_aborted)
                {
                    spdlog::warn("Waiting for registry WAL sync worker stop failed: {}", wait_ec.message());
                }
                break;
            }
        }

        spdlog::info("Registry WAL sync worker stopped");
    }

    if(wal_enabled)
    {
        spdlog::info("Running final registry WAL truncate checkpoint...");
        const auto stats = co_await registry.checkpointWal(dcn::storage::sqlite::WalCheckpointMode::TRUNCATE);
        if(!stats.ok)
        {
            spdlog::warn("Final registry WAL truncate checkpoint failed");
        }
        else
        {
            spdlog::info("Final registry WAL truncate checkpoint complete");
        }
    }

    co_return;
}

static void _runImmediateShutdown(
    std::optional<dcn::storage::sqlite::WalSyncWorker> & wal_sync_worker,
    dcn::events::EventRuntime & events_runtime,
    dcn::feed::FeedRuntime & feed_runtime,
    dcn::registry::ArtifactRuntime & artifact_runtime)
{
    artifact_runtime.requestStop();
    spdlog::info("Artifact runtime stop requested");
    feed_runtime.requestStop();
    spdlog::info("Feed runtime stop requested");
    events_runtime.requestStop();
    spdlog::info("Events runtime stop requested");

    if(wal_sync_worker)
    {
        spdlog::info("Registry WAL sync worker stopping...");
        wal_sync_worker->requestStop();
        spdlog::info("Stopped registry WAL sync worker");
    }
}

namespace
{
    // What the server proved about the configured chain deployment before using it.
    struct ChainDeployment
    {
        std::optional<dcn::publish::DeploymentManifest> manifest;
        std::string registry;
        std::string runner;   // empty when neither the manifest nor --chain-runner names one
        std::uint64_t start_block = 0;
    };
}

// Deploys registry and runner with the bootstrap key and persists their manifest.
// Automatic deployment is for private and development chains: a canonical public
// deployment is an explicit release operation whose reviewed manifest is committed.
static asio::awaitable<std::expected<dcn::publish::DeploymentManifest, std::string>> _bootstrapDeployment(
    const dcn::rpc::EthClient & client,
    dcn::evm::EVM & evm,
    const dcn::config::Config & cfg,
    const std::filesystem::path & pt_path)
{
    const auto signer = cfg.publish.bootstrap_key_file.empty()
        ? dcn::publish::Signer::fromEnvironment(cfg.publish.bootstrap_key_env)
        : dcn::publish::Signer::fromKeyFile(cfg.publish.bootstrap_key_file);
    if(!signer)
    {
        co_return std::unexpected(std::format("cannot load the bootstrap key: {}", signer.error()));
    }
    spdlog::info("Bootstrapping a PT deployment from account {}", dcn::chain::addressToHex((*signer)->address()));

    // Compiled into their own directory so the bootstrap never races the local EVM's
    // own PT build.
    const auto pt_out = cfg.storage_path / "pt-bootstrap";
    const auto contracts = pt_path / "contracts";
    std::error_code dir_ec;
    std::filesystem::create_directories(pt_out, dir_ec);
    if(dir_ec)
    {
        co_return std::unexpected(std::format("cannot create '{}': {}", pt_out.string(), dir_ec.message()));
    }

    // A named array rather than a braced range: GCC cannot compile an initializer_list
    // that lives across a co_await.
    const std::array<std::filesystem::path, 4> sources{
        std::filesystem::path("registry") / "RegistryBase.sol",
        std::filesystem::path("proxy") / "PTRegistryProxy.sol",
        std::filesystem::path("runner") / "Runner.sol",
        std::filesystem::path("proxy") / "PTContractProxy.sol"};
    for(const auto & source : sources)
    {
        if(!co_await evm.compile(contracts / source, pt_out, contracts, pt_path / "node_modules"))
        {
            co_return std::unexpected(std::format("cannot compile '{}' for deployment", source.string()));
        }
    }

    auto deployed = co_await dcn::publish::bootstrapDeployment(
        client,
        **signer,
        dcn::publish::BootstrapArtifacts{
            .registry_impl_bin = pt_out / "RegistryBase.bin",
            .registry_proxy_bin = pt_out / "PTRegistryProxy.bin",
            .runner_impl_bin = pt_out / "Runner.bin",
            .runner_proxy_bin = pt_out / "PTContractProxy.bin"},
        cfg.events_chain_id,
        evm.solcVersion(),
        dcn::loader::ptSourceRevision(pt_path),
        dcn::publish::BootstrapConfig{
            .max_gas = cfg.publish.bootstrap_max_gas,
            .max_fee_per_gas_wei = cfg.publish.bootstrap_max_fee_per_gas_wei});
    if(!deployed)
    {
        co_return std::unexpected(deployed.error());
    }

    const auto written = dcn::publish::writeManifest(cfg.publish.manifest_path, *deployed);
    if(!written)
    {
        // The contracts exist on chain now; the operator needs their addresses to write
        // the manifest by hand instead of deploying a second time.
        co_return std::unexpected(std::format(
            "cannot persist the deployment manifest ({}); registry {} and runner {} were deployed",
            written.error(), deployed->registry, deployed->runner));
    }
    co_return std::move(*deployed);
}

// Positively identifies the configured deployment before a single row is ingested from
// it or a publication is prepared against it: a wrong chain or a non-PT proxy is a
// configuration error, not something to discover halfway through a backfill.
static asio::awaitable<std::expected<ChainDeployment, std::string>> _resolveChainDeployment(
    const dcn::rpc::EthClient & client,
    dcn::evm::EVM & evm,
    const dcn::config::Config & cfg,
    const std::filesystem::path & pt_path)
{
    const auto & ingestion = cfg.chain_ingestion;

    const auto chain_id = co_await client.chainId();
    if(!chain_id)
    {
        co_return std::unexpected(std::format("eth_chainId failed: {}", chain_id.error().message));
    }
    if(*chain_id != cfg.events_chain_id)
    {
        co_return std::unexpected(std::format(
            "--events-chain-id {} does not match endpoint chain id {}", cfg.events_chain_id, *chain_id));
    }

    // A manifest is the only way a server learns a canonical deployment; a chain cannot
    // be searched for "the" PT registry. An unreadable manifest is an error, never a
    // reason to bootstrap a second deployment.
    std::optional<dcn::publish::DeploymentManifest> manifest;
    if(std::filesystem::exists(cfg.publish.manifest_path))
    {
        auto stored = dcn::publish::readManifest(cfg.publish.manifest_path);
        if(!stored)
        {
            co_return std::unexpected(std::format("cannot read the deployment manifest: {}", stored.error()));
        }
        manifest = std::move(*stored);
    }
    else if(ingestion.registry_address.empty())
    {
        if(!cfg.publish.bootstrap)
        {
            co_return std::unexpected(
                "no PT deployment is configured: pass --chain-registry, provide a deployment "
                "manifest, or run with --publish-bootstrap to deploy one");
        }
        auto deployed = co_await _bootstrapDeployment(client, evm, cfg, pt_path);
        if(!deployed) co_return std::unexpected(deployed.error());
        manifest = std::move(*deployed);
    }

    ChainDeployment deployment;
    if(manifest)
    {
        if(manifest->chain_id != cfg.events_chain_id)
        {
            co_return std::unexpected(std::format(
                "the deployment manifest is for chain {}, not --events-chain-id {}",
                manifest->chain_id, cfg.events_chain_id));
        }
        if(!ingestion.registry_address.empty() && ingestion.registry_address != manifest->registry)
        {
            co_return std::unexpected(std::format(
                "--chain-registry {} does not match the manifest registry {}",
                ingestion.registry_address, manifest->registry));
        }
        if(!ingestion.runner_address.empty() && ingestion.runner_address != manifest->runner)
        {
            co_return std::unexpected(std::format(
                "--chain-runner {} does not match the manifest runner {}",
                ingestion.runner_address, manifest->runner));
        }

        const auto verified = co_await dcn::publish::verifyDeployment(client, *manifest);
        if(!verified) co_return std::unexpected(verified.error());

        deployment.registry = manifest->registry;
        deployment.runner = manifest->runner;
        deployment.start_block =
            ingestion.start_block.value_or(static_cast<std::uint64_t>(manifest->deployment_block));
        deployment.manifest = std::move(manifest);
        co_return deployment;
    }

    // Configured by address only: identity is proven, but no implementation is pinned.
    const auto registry = co_await dcn::rpc::verifyPtRegistry(client, cfg.events_chain_id, ingestion.registry_address);
    if(!registry) co_return std::unexpected(registry.error());
    if(!ingestion.runner_address.empty())
    {
        const auto runner = co_await dcn::rpc::verifyPtRunner(client, ingestion.runner_address, ingestion.registry_address);
        if(!runner) co_return std::unexpected(runner.error());
    }

    if(!ingestion.start_block)
    {
        co_return std::unexpected(
            "--chain-start-block is required when no deployment manifest supplies a deployment "
            "block: scanning from genesis is prohibitively expensive and skipping history would "
            "silently lose events");
    }

    deployment.registry = ingestion.registry_address;
    deployment.runner = ingestion.runner_address;
    deployment.start_block = *ingestion.start_block;
    co_return deployment;
}

int main(int argc, char* argv[])
{
    dcn::config::Config cfg;
    cfg.bin_path = std::filesystem::path(argv[0]).parent_path();
    cfg.logs_path = cfg.bin_path.parent_path() / "logs";
    cfg.resources_path = cfg.bin_path.parent_path() / "resources";
    cfg.storage_path = cfg.bin_path.parent_path() / "storage";

    const bool terminal_configured = dcn::native::configureTerminal();
    _configureLogger(cfg.logs_path);

    if(!terminal_configured)
    {
        std::printf("%s", dcn::utils::getLogo(dcn::utils::LogoASCII).c_str());
        std::fflush(stdout);
        spdlog::warn("Terminal configuration was not fully applied");
    }
    else
    {
        std::printf("%s", dcn::utils::getLogo(dcn::utils::LogoUnicode).c_str());
        std::fflush(stdout);
        spdlog::debug("Terminal configuration applied successfully");
    }

    const std::string build_timestamp = dcn::utils::loadBuildTimestamp(cfg.bin_path / "build_timestamp");
    spdlog::debug("Build timestamp: {}", build_timestamp);

    spdlog::debug("Version: {}.{}.{}\n", dcn::MAJOR_VERSION, dcn::MINOR_VERSION, dcn::PATCH_VERSION);

    spdlog::debug("Decentralised Art server started with {} arguments", argc);
    
    dcn::cmd::ArgParser arg_parser;
    arg_parser.addArg<bool>("-h", "Display help message and exit");
    arg_parser.addArg<bool>("--help", "Display help message and exit");
    arg_parser.addArg<bool>("--version", "Display version and exit");
    arg_parser.addArg<bool>("--verbose", "Enable verbose logging");
    arg_parser.addArg<unsigned int>("--port", "Port to listen on");
    arg_parser.addArg<std::string>("--chain-rpc", "Ethereum JSON-RPC endpoint URL used for event sync, execution and publication");
    arg_parser.addArg<std::string>("--chain-rpc-env", "Environment variable holding the JSON-RPC endpoint URL; keeps a provider API key out of the process table");
    arg_parser.addArg<std::string>("--chain-registry", "PT registry proxy address, when no deployment manifest is used");
    arg_parser.addArg<std::string>("--chain-runner", "PT runner proxy address, when no deployment manifest is used");
    arg_parser.addArg<unsigned int>("--chain-start-block", "First block for event sync when no local cursor exists; defaults to the manifest deployment block");
    arg_parser.addArg<unsigned int>("--chain-poll-ms", "Chain poll interval in milliseconds");
    arg_parser.addArg<unsigned int>("--chain-confirmations", "Finality depth used while the endpoint serves no safe/finalized block tags");
    arg_parser.addArg<unsigned int>("--chain-batch-size", "Max number of blocks fetched per eth_getLogs request");
    arg_parser.addArg<bool>("--chain-local-source", "Use in-process EVM as chain event source (no RPC)");
    arg_parser.addArg<unsigned int>("--execute-max-gas", "Gas cap of the runner eth_call POST /execute makes for chain connectors");
    arg_parser.addArg<std::string>("--execute-block-tag", "Block POST /execute reads chain state at: safe (default), finalized or latest");
    arg_parser.addArg<std::filesystem::path>("--registry-db", "SQLite path for registry storage");
    arg_parser.addArg<unsigned int>("--registry-wal-sync-ms", "Interval in milliseconds for periodic SQLite WAL passive checkpoints");
    arg_parser.addArg<std::filesystem::path>("--events-db", "SQLite path for events hot storage");
    arg_parser.addArg<std::filesystem::path>("--feed-db", "SQLite path for feed storage");
    arg_parser.addArg<std::filesystem::path>("--feed-archive-root", "Directory root for feed archive segments");
    arg_parser.addArg<std::string>("--events-chain-id", "Chain id used for events ingestion and feed projection");
    arg_parser.addArg<unsigned int>("--events-hot-window-days", "Retention window in days for hot events storage");
    arg_parser.addArg<unsigned int>("--events-projector-ms", "Interval in milliseconds for projector loop when idle");
    arg_parser.addArg<unsigned int>("--events-archive-ms", "Interval in milliseconds for archive maintenance loop");
    arg_parser.addArg<unsigned int>("--events-reorg-window-blocks", "Rolling block window size for reorg reconciliation");
    arg_parser.addArg<unsigned int>("--events-outbox-retention-days", "Retention window in days for replay outbox rows");
    arg_parser.addArg<unsigned int>("--events-projector-retry-attempts", "Consecutive failing projector passes on a row before it is dead-lettered");
    arg_parser.addArg<unsigned int>("--events-dead-letter-sweep-ms", "Interval in milliseconds between idle-time dead-letter retry sweeps");
    arg_parser.addArg<bool>("--publish", "Enable the /publish/* routes that prepare and confirm owner-paid publications");
    arg_parser.addArg<std::filesystem::path>("--deployment-manifest", "Path to the PT deployment manifest");
    arg_parser.addArg<bool>("--publish-bootstrap", "Deploy registry and runner with the bootstrap key when no deployment is configured");
    arg_parser.addArg<std::filesystem::path>("--bootstrap-key-file", "File holding the bootstrap deployer private key in hex");
    arg_parser.addArg<std::string>("--bootstrap-key-env", "Environment variable holding the bootstrap deployer private key");
    arg_parser.addArg<unsigned int>("--bootstrap-max-gas", "Maximum gas the bootstrap deployer signs for one transaction");
    arg_parser.addArg<unsigned int>("--bootstrap-max-fee-gwei", "Maximum total fee per gas the bootstrap deployer signs");
    arg_parser.addArg<std::string>("--artifact-gateways", "Semicolon-separated artifact gateways: http(s) URLs or directories");
    arg_parser.addArg<std::string>("--artifact-replicas", "Semicolon-separated durable artifact replica directories");

    arg_parser.parse(argc, argv);

    if(arg_parser.getArg<bool>("--version").value_or(false))
    {
        spdlog::info("Decentralised Art server build timestamp: {}", build_timestamp);
        spdlog::info("Version: {}.{}.{}", dcn::MAJOR_VERSION, dcn::MINOR_VERSION, dcn::PATCH_VERSION);
        return 0;
    }

    if(arg_parser.getArg<bool>("--help").value_or(false) || arg_parser.getArg<bool>("-h").value_or(false))
    {
        std::printf("%s", arg_parser.constructHelpMessage().c_str());
        return 0;
    }
    
    cfg.verbose = arg_parser.getArg<bool>("--verbose").value_or(false);

    cfg.port = arg_parser.getArg<unsigned int>("--port").value_or(dcn::DEFAULT_PORT);

    const auto events_chain_id = _parseUint64(arg_parser.getArg<std::string>("--events-chain-id").value_or("1"));
    if(!events_chain_id || *events_chain_id == 0)
    {
        spdlog::error("--events-chain-id must be a positive integer");
        return 1;
    }
    cfg.events_chain_id = *events_chain_id;

    cfg.chain_ingestion.poll_interval_ms = arg_parser.getArg<unsigned int>("--chain-poll-ms").value_or(5000);
    cfg.chain_ingestion.confirmations = arg_parser.getArg<unsigned int>("--chain-confirmations").value_or(12);
    cfg.chain_ingestion.block_batch_size = arg_parser.getArg<unsigned int>("--chain-batch-size").value_or(500);
    if(const auto start_block_arg = arg_parser.getArg<unsigned int>("--chain-start-block"))
    {
        cfg.chain_ingestion.start_block = static_cast<std::uint64_t>(*start_block_arg);
    }
    cfg.chain_ingestion.use_local_evm_source = arg_parser.getArg<bool>("--chain-local-source").value_or(false);

    const auto rpc_url_arg = arg_parser.getArg<std::string>("--chain-rpc");
    const auto rpc_env_arg = arg_parser.getArg<std::string>("--chain-rpc-env");
    if(rpc_url_arg && rpc_env_arg)
    {
        spdlog::error("Pass either --chain-rpc or --chain-rpc-env, not both");
        return 1;
    }
    if(rpc_env_arg)
    {
        const char * rpc_url = std::getenv(rpc_env_arg->c_str());
        if(rpc_url == nullptr || *rpc_url == '\0')
        {
            spdlog::error("The environment variable named by --chain-rpc-env is not set");
            return 1;
        }
        cfg.chain_ingestion.rpc_url = rpc_url;
    }
    else
    {
        cfg.chain_ingestion.rpc_url = rpc_url_arg.value_or("");
    }
    const bool has_chain_rpc = !cfg.chain_ingestion.rpc_url.empty();

    for(const auto & [flag, target] : {
        std::pair{"--chain-registry", &cfg.chain_ingestion.registry_address},
        std::pair{"--chain-runner", &cfg.chain_ingestion.runner_address}})
    {
        const auto value = arg_parser.getArg<std::string>(flag);
        if(!value) continue;
        const auto address = _parseAddress(*value);
        if(!address)
        {
            spdlog::error("Invalid {} address", flag);
            return 1;
        }
        *target = *address;
    }

    cfg.execute.max_gas = arg_parser.getArg<unsigned int>("--execute-max-gas").value_or(100'000'000);
    cfg.execute.block_tag = arg_parser.getArg<std::string>("--execute-block-tag").value_or(std::string(dcn::rpc::BLOCK_TAG_SAFE));
    if(cfg.execute.block_tag != dcn::rpc::BLOCK_TAG_SAFE
       && cfg.execute.block_tag != dcn::rpc::BLOCK_TAG_FINALIZED
       && cfg.execute.block_tag != dcn::rpc::BLOCK_TAG_LATEST)
    {
        spdlog::error("--execute-block-tag must be safe, finalized or latest");
        return 1;
    }

    cfg.publish.enabled = arg_parser.getArg<bool>("--publish").value_or(false);
    cfg.publish.manifest_path = arg_parser.getArg<std::filesystem::path>("--deployment-manifest").value_or(
        cfg.storage_path / "deployment.json");
    cfg.publish.bootstrap = arg_parser.getArg<bool>("--publish-bootstrap").value_or(false);
    cfg.publish.bootstrap_key_file = arg_parser.getArg<std::filesystem::path>("--bootstrap-key-file").value_or("");
    cfg.publish.bootstrap_key_env = arg_parser.getArg<std::string>("--bootstrap-key-env").value_or("DCN_BOOTSTRAP_KEY");
    cfg.publish.bootstrap_max_gas = arg_parser.getArg<unsigned int>("--bootstrap-max-gas").value_or(12'000'000);
    cfg.publish.bootstrap_max_fee_per_gas_wei =
        static_cast<std::uint64_t>(arg_parser.getArg<unsigned int>("--bootstrap-max-fee-gwei").value_or(200))
        * 1'000'000'000ULL;

    if(!has_chain_rpc)
    {
        for(const auto & [flag, given] : {
            std::pair{"--chain-registry", !cfg.chain_ingestion.registry_address.empty()},
            std::pair{"--chain-runner", !cfg.chain_ingestion.runner_address.empty()},
            std::pair{"--publish", cfg.publish.enabled},
            std::pair{"--publish-bootstrap", cfg.publish.bootstrap}})
        {
            if(given)
            {
                spdlog::error("{} requires --chain-rpc or --chain-rpc-env", flag);
                return 1;
            }
        }
    }

    cfg.registry_wal_sync_ms = arg_parser.getArg<unsigned int>("--registry-wal-sync-ms").value_or(30000);

    const std::chrono::milliseconds registry_wal_sync_interval(cfg.registry_wal_sync_ms);

    cfg.registry_db = arg_parser.getArg<std::filesystem::path>("--registry-db").value_or(
        cfg.storage_path / "registry.sqlite"
    );

    cfg.events_db = arg_parser.getArg<std::filesystem::path>("--events-db").value_or(
        cfg.storage_path / "events" / "events_hot.sqlite"
    );
    cfg.feed_db = arg_parser.getArg<std::filesystem::path>("--feed-db").value_or(
        cfg.storage_path / "feed" / "feed.sqlite"
    );
    cfg.feed_archive_root = arg_parser.getArg<std::filesystem::path>("--feed-archive-root").value_or(
        cfg.storage_path / "feed" / "archive"
    );
    cfg.events_hot_window_days = arg_parser.getArg<unsigned int>("--events-hot-window-days").value_or(90);
    cfg.events_projector_interval_ms = arg_parser.getArg<unsigned int>("--events-projector-ms").value_or(200);
    cfg.events_archive_interval_ms = arg_parser.getArg<unsigned int>("--events-archive-ms").value_or(30000);
    cfg.events_reorg_window_blocks = arg_parser.getArg<unsigned int>("--events-reorg-window-blocks").value_or(2048);
    cfg.events_outbox_retention_days = arg_parser.getArg<unsigned int>("--events-outbox-retention-days").value_or(7);
    cfg.events_projector_retry_attempts = arg_parser.getArg<unsigned int>("--events-projector-retry-attempts").value_or(5);
    cfg.events_dead_letter_sweep_ms = arg_parser.getArg<unsigned int>("--events-dead-letter-sweep-ms").value_or(60'000);
    cfg.artifact_gateways = _splitList(arg_parser.getArg<std::string>("--artifact-gateways").value_or(""));
    for(const std::string & replica : _splitList(arg_parser.getArg<std::string>("--artifact-replicas").value_or("")))
    {
        cfg.artifact_replicas.emplace_back(replica);
    }
    if(cfg.publish.enabled && cfg.artifact_replicas.empty())
    {
        // A publication whose artifact is not durably stored is indexable but not
        // reconstructable, so preparing one is refused.
        spdlog::error("--publish requires --artifact-replicas");
        return 1;
    }

    spdlog::info("Current working path: {}", std::filesystem::current_path().string());

    // solidity check
    const auto solc_path = cfg.bin_path / TOSTRING(Solidity_SOLC_EXECUTABLE);

    spdlog::info(std::format("Path to solidity solc compiler : {}", solc_path.string()));

    const auto [exit_code, solc_version_out] = dcn::native::runProcess(solc_path.string(), {"--version"});
    spdlog::info(std::format("Solc info:\n{}", solc_version_out));
    
    const auto pt_path = cfg.bin_path.parent_path() / "pt";
    spdlog::info(std::format("Path to PT framework : {}", pt_path.string()));

    const bool registry_db_in_memory = cfg.registry_db.empty() || cfg.registry_db == ":memory:";

    // create directories
    if(!_createDirectories(cfg))
    {
        return 1;
    }

    asio::io_context io_context;

    dcn::registry::Registry registry(io_context, cfg.registry_db.string());

    // A gateway given as a URL is fetched over HTTP(S); anything else is a directory.
    std::vector<std::filesystem::path> gateway_dirs;
    std::vector<std::shared_ptr<dcn::rpc::IHttpClient>> gateway_clients;
    for(const std::string & gateway : cfg.artifact_gateways)
    {
        if(gateway.find("://") == std::string::npos)
        {
            gateway_dirs.emplace_back(gateway);
            continue;
        }
        auto endpoint = dcn::rpc::parseEndpoint(gateway);
        if(!endpoint)
        {
            spdlog::error("Invalid artifact gateway URL: only http:// and https:// are supported");
            return 1;
        }
        gateway_clients.push_back(dcn::rpc::makeHttpClient(io_context, std::move(*endpoint)));
    }

    const dcn::pt::ArtifactStore artifact_store(
        cfg.storage_path / "artifacts" / "cache",
        std::move(gateway_dirs),
        cfg.artifact_replicas);
    dcn::registry::ArtifactRuntime artifact_runtime(io_context, registry, artifact_store, std::move(gateway_clients));

    dcn::auth::AuthManager auth_manager(io_context);

    dcn::evm::EVM evm(io_context, EVMC_SHANGHAI, solc_path, pt_path);

    dcn::server::Server server(io_context, {asio::ip::tcp::v4(), asio::ip::port_type(cfg.port)});

    server.setIdleInterval(5000ms);

    std::vector<std::shared_ptr<dcn::events::IEmittedLogSource>> event_sources;
    if(cfg.chain_ingestion.use_local_evm_source)
    {
        event_sources.push_back(std::make_shared<dcn::events::LocalEvmSource>(evm, cfg.events_chain_id));
        cfg.chain_ingestion.enabled = true;
    }

    std::optional<dcn::ChainAccess> chain_access;
    std::optional<dcn::publish::Publisher> publisher;

    if(has_chain_rpc)
    {
        auto transport = dcn::rpc::makeHttpTransport(io_context, cfg.chain_ingestion.rpc_url);
        if(!transport)
        {
            spdlog::error("Invalid chain RPC URL: only http:// and https:// are supported");
            return 1;
        }
        auto eth_client = std::make_shared<dcn::rpc::EthClient>(
            std::make_shared<dcn::rpc::JsonRpcClient>(std::move(transport)));

        std::optional<std::expected<ChainDeployment, std::string>> resolved;
        asio::co_spawn(
            io_context,
            [&]() -> asio::awaitable<void>
            {
                resolved = co_await _resolveChainDeployment(*eth_client, evm, cfg, pt_path);
            },
            asio::detached);
        io_context.run();
        io_context.restart();

        if(!resolved)
        {
            // The io_context stopped before the checks finished, so nothing about the
            // deployment has been proven.
            spdlog::error("Deployment verification did not complete");
            return 1;
        }
        if(!*resolved)
        {
            spdlog::error("Cannot use the configured chain deployment: {}", resolved->error());
            return 1;
        }
        ChainDeployment & deployment = **resolved;

        spdlog::info(
            "PT registry {} verified on chain {} from block {}",
            deployment.registry,
            cfg.events_chain_id,
            deployment.start_block);
        if(deployment.runner.empty())
        {
            spdlog::warn(
                "No PT runner is configured: POST /execute is unavailable. "
                "Pass --chain-runner or use a deployment manifest.");
        }

        // Source identity is chain plus registry, never the provider URL: moving to
        // another provider for the same deployment must resume the same cursor rather
        // than start a second logical chain.
        event_sources.push_back(std::make_shared<dcn::events::RpcLogSource>(
            eth_client,
            dcn::events::RpcLogSourceConfig{
                .source_id = std::format("eip155:{}:{}", cfg.events_chain_id, deployment.registry),
                .chain_namespace = std::format("eip155:{}", cfg.events_chain_id),
                .chain_id = cfg.events_chain_id,
                .registry_address = deployment.registry,
                .deployment_block = static_cast<std::int64_t>(deployment.start_block),
                .max_batch_blocks = static_cast<std::int64_t>(cfg.chain_ingestion.block_batch_size),
                .confirmations = cfg.chain_ingestion.confirmations,
                .expected_implementation_code_hash = deployment.manifest
                    ? deployment.manifest->registry_impl_code_hash
                    : std::string{}
            }));
        cfg.chain_ingestion.enabled = true;

        chain_access = dcn::ChainAccess{
            .client = eth_client,
            .registry = deployment.registry,
            .runner = deployment.runner,
            .execute_max_gas = cfg.execute.max_gas,
            .execute_block_tag = cfg.execute.block_tag};

        if(cfg.publish.enabled)
        {
            publisher.emplace(
                eth_client,
                deployment.manifest.value_or(dcn::publish::DeploymentManifest{
                    .chain_id = cfg.events_chain_id,
                    .registry = deployment.registry,
                    .runner = deployment.runner,
                    .protocol_id = dcn::rpc::ptProtocolId(),
                    .protocol_version = dcn::rpc::PT_PROTOCOL_VERSION}),
                artifact_store);
        }
    }

    dcn::events::EventRuntime events_runtime(
        io_context,
        dcn::events::EventRuntimeConfig{
            .hot_db_path = cfg.events_db,
            .chain_id = cfg.events_chain_id,
            .ingestion_enabled = cfg.chain_ingestion.enabled,
            .sources = std::move(event_sources),
            .poll_interval_ms = cfg.chain_ingestion.poll_interval_ms,
            .block_batch_size = cfg.chain_ingestion.block_batch_size,
            .reorg_window_blocks = static_cast<std::size_t>(cfg.events_reorg_window_blocks),
            .projector_interval_ms = cfg.events_projector_interval_ms
        });

    dcn::feed::FeedRuntime feed_runtime(
        io_context,
        dcn::feed::FeedRuntimeConfig{
            .feed_db_path   = cfg.feed_db,
            .archive_root   = cfg.feed_archive_root,
            .hot_window_days = cfg.events_hot_window_days,
            .outbox_retention_ms = static_cast<std::int64_t>(cfg.events_outbox_retention_days) * 24LL * 60LL * 60LL * 1000LL,
            .archive_interval_ms = cfg.events_archive_interval_ms
        });

    const auto favicon = dcn::file::loadBinaryFile(cfg.resources_path / "media" / "img" / "favicon.svg");
    
    // HTML
    const auto simple_form_html = dcn::file::loadTextFile(cfg.resources_path / "html" / "simple_form.html");
    
    // JS
    const auto simple_form_js = dcn::file::loadTextFile(cfg.resources_path / "js" / "simple_form.js");
    const auto auth_js = dcn::file::loadTextFile(cfg.resources_path / "js" / "auth.js");
    const auto execute_js = dcn::file::loadTextFile(cfg.resources_path / "js" / "execute.js");
    const auto publish_js = dcn::file::loadTextFile(cfg.resources_path / "js" / "publish.js");
    const auto utils_js = dcn::file::loadTextFile(cfg.resources_path / "js" / "utils.js");

    // CSS
    const auto simple_form_css = dcn::file::loadTextFile(cfg.resources_path / "styles" / "simple_form.css");

    if(simple_form_html && simple_form_js && auth_js && execute_js && publish_js && utils_js && simple_form_css)
    {
        server.addRoute({dcn::http::Method::HEAD, "/"},     dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/"},  dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/"},      dcn::GET_serveFile, "text/html; charset=utf-8", std::cref(simple_form_html.value()));

        server.addRoute({dcn::http::Method::HEAD, "/js/simple_form"},    dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/js/simple_form"}, dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/js/simple_form"},     dcn::GET_serveFile, "text/javascript; charset=utf-8", std::cref(simple_form_js.value()));
    
        server.addRoute({dcn::http::Method::HEAD, "/js/auth"},    dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/js/auth"}, dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/js/auth"},     dcn::GET_serveFile, "text/javascript; charset=utf-8", std::cref(auth_js.value()));

        server.addRoute({dcn::http::Method::HEAD, "/js/execute"},    dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/js/execute"}, dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/js/execute"},     dcn::GET_serveFile, "text/javascript; charset=utf-8", std::cref(execute_js.value()));

        server.addRoute({dcn::http::Method::HEAD, "/js/publish"},    dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/js/publish"}, dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/js/publish"},     dcn::GET_serveFile, "text/javascript; charset=utf-8", std::cref(publish_js.value()));

        server.addRoute({dcn::http::Method::HEAD, "/js/utils"},    dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/js/utils"}, dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/js/utils"},     dcn::GET_serveFile, "text/javascript; charset=utf-8", std::cref(utils_js.value()));

        server.addRoute({dcn::http::Method::HEAD, "/styles/simple_form.css"},       dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/styles/simple_form.css"},    dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/styles/simple_form.css"},        dcn::GET_serveFile, "text/css; charset=utf-8", std::cref(simple_form_css.value()));
    }
    else
    {
        spdlog::error("Failed to load static files");
    }

    if(favicon)
    {
        server.addRoute({dcn::http::Method::HEAD, "/favicon.svg"},      dcn::HEAD_serveFile);
        server.addRoute({dcn::http::Method::OPTIONS, "/favicon.svg"},   dcn::OPTIONS_serveFile);
        server.addRoute({dcn::http::Method::GET, "/favicon.svg"},       dcn::GET_serveBinaryFile, "image/svg+xml; charset=utf-8", std::cref(favicon.value()));
    }
    else
    {
        spdlog::error("Failed to load favicon");
    }
    
    server.addRoute({dcn::http::Method::GET, "/version"},   dcn::GET_version, std::cref(build_timestamp));

    server.addRoute({dcn::http::Method::GET, "/nonce/<string>"},    dcn::GET_nonce, std::ref(auth_manager));

    server.addRoute({dcn::http::Method::OPTIONS, "/auth"},  dcn::OPTIONS_auth);
    server.addRoute({dcn::http::Method::POST, "/auth"},     dcn::POST_auth, std::ref(auth_manager));

    server.addRoute(
        {dcn::http::Method::OPTIONS, "/account/<string>?limit=<uint>&after_connectors=<~string>&after_transformations=<~string>&after_conditions=<~string>"},
        dcn::OPTIONS_accountInfo);
    server.addRoute(
        {dcn::http::Method::GET, "/account/<string>?limit=<uint>&after_connectors=<~string>&after_transformations=<~string>&after_conditions=<~string>"},
        dcn::GET_accountInfo,
        std::ref(registry));
    server.addRoute({dcn::http::Method::OPTIONS, "/accounts?limit=<uint>&after=<~string>"}, dcn::OPTIONS_accounts);
    server.addRoute({dcn::http::Method::HEAD, "/accounts?limit=<uint>&after=<~string>"}, dcn::HEAD_accounts, std::ref(registry));
    server.addRoute({dcn::http::Method::GET, "/accounts?limit=<uint>&after=<~string>"}, dcn::GET_accounts, std::ref(registry));

    server.addRoute({dcn::http::Method::OPTIONS, "/formats?limit=<uint>&after=<~string>"}, dcn::OPTIONS_formats);
    server.addRoute({dcn::http::Method::HEAD, "/formats?limit=<uint>&after=<~string>"}, dcn::HEAD_formats, std::ref(registry));
    server.addRoute({dcn::http::Method::GET, "/formats?limit=<uint>&after=<~string>"}, dcn::GET_formats, std::ref(registry));
    server.addRoute({dcn::http::Method::OPTIONS, "/format/<string>?limit=<uint>&after=<~string>"}, dcn::OPTIONS_format);
    server.addRoute({dcn::http::Method::GET, "/format/<string>?limit=<uint>&after=<~string>"}, dcn::GET_format, std::ref(registry));

    server.addRoute({dcn::http::Method::OPTIONS, "/feed?limit=<uint>&before=<~string>&type=<~string>&include_unfinalized=<~uint>"}, dcn::OPTIONS_feed);
    server.addRoute({dcn::http::Method::GET, "/feed?limit=<uint>&before=<~string>&type=<~string>&include_unfinalized=<~uint>"}, dcn::GET_feed, std::ref(static_cast<dcn::feed::IFeedRepository &>(feed_runtime.feed())));
    server.addRoute({dcn::http::Method::OPTIONS, "/feed/stream?since_seq=<~uint>&limit=<~uint>"}, dcn::OPTIONS_feedStream);
    server.addStreamingRoute({dcn::http::Method::GET, "/feed/stream?since_seq=<~uint>&limit=<~uint>"}, dcn::GET_feedStream, std::ref(static_cast<dcn::feed::IFeedRepository &>(feed_runtime.feed())));

    const dcn::ChainAccess * chain_ptr = chain_access ? &*chain_access : nullptr;

    server.addRoute({dcn::http::Method::HEAD, "/connector/<string>"},       dcn::HEAD_connector, std::ref(registry));
    server.addRoute({dcn::http::Method::OPTIONS, "/connector"},             dcn::OPTIONS_connector);
    server.addRoute({dcn::http::Method::OPTIONS, "/connector/<string>"},    dcn::OPTIONS_connector);
    server.addRoute({dcn::http::Method::GET,     "/connector/<string>"},    dcn::GET_connector, std::ref(registry));
    server.addRoute({dcn::http::Method::POST,    "/connector"},             dcn::POST_connector, std::ref(auth_manager), std::ref(registry), std::ref(evm), std::cref(cfg), chain_ptr);

    server.addRoute({dcn::http::Method::HEAD, "/transformation/<string>"},    dcn::HEAD_transformation, std::ref(registry));
    server.addRoute({dcn::http::Method::OPTIONS, "/transformation"},          dcn::OPTIONS_transformation);
    server.addRoute({dcn::http::Method::OPTIONS, "/transformation/<string>"}, dcn::OPTIONS_transformation);
    server.addRoute({dcn::http::Method::GET,     "/transformation/<string>"}, dcn::GET_transformation, std::ref(registry));
    server.addRoute({dcn::http::Method::POST,    "/transformation"},          dcn::POST_transformation, std::ref(auth_manager), std::ref(registry), std::ref(evm), std::cref(cfg), chain_ptr);

    server.addRoute({dcn::http::Method::HEAD, "/condition/<string>"},    dcn::HEAD_condition, std::ref(registry));
    server.addRoute({dcn::http::Method::OPTIONS, "/condition"},          dcn::OPTIONS_condition);
    server.addRoute({dcn::http::Method::OPTIONS, "/condition/<string>"}, dcn::OPTIONS_condition);
    server.addRoute({dcn::http::Method::GET,     "/condition/<string>"}, dcn::GET_condition, std::ref(registry));
    server.addRoute({dcn::http::Method::POST,    "/condition"},          dcn::POST_condition, std::ref(auth_manager), std::ref(registry), std::ref(evm), std::cref(cfg), chain_ptr);

    const dcn::publish::Publisher * publisher_ptr = publisher ? &*publisher : nullptr;

    // Registered unconditionally so a client learns publication is disabled from a 403
    // rather than a 404 that looks like a wrong URL.
    server.addRoute({dcn::http::Method::OPTIONS, "/publish/<string>/prepare"}, dcn::OPTIONS_publish);
    server.addRoute({dcn::http::Method::POST, "/publish/<string>/prepare"},
        dcn::POST_publishPrepare, std::ref(auth_manager), std::ref(evm), publisher_ptr, std::cref(cfg));
    server.addRoute({dcn::http::Method::OPTIONS, "/publish/<string>"}, dcn::OPTIONS_publish);
    server.addRoute({dcn::http::Method::POST, "/publish/<string>"},
        dcn::POST_publishConfirm, std::ref(auth_manager), publisher_ptr);

    server.addRoute({dcn::http::Method::OPTIONS, "/execute"},   dcn::OPTIONS_execute);
    server.addRoute({dcn::http::Method::POST, "/execute"},      dcn::POST_execute, std::cref(auth_manager), chain_ptr);
    server.addRoute({dcn::http::Method::OPTIONS, "/simulate"},  dcn::OPTIONS_execute);
    server.addRoute({dcn::http::Method::POST, "/simulate"},     dcn::POST_simulate, std::cref(auth_manager), std::ref(registry), std::ref(evm), std::cref(cfg));

    if(!dcn::loader::ensurePTBuildVersion(cfg.storage_path))
    {
        spdlog::error("Failed to prepare PT Solidity build cache");
        return 1;
    }

    // Wire both projectors into the EventRuntime before start() — symmetric construction.
    // feed::FeedProjector first so it runs first in every projector loop iteration.
    const dcn::events::ProjectorRetryConfig projector_retry{
        .max_attempts = cfg.events_projector_retry_attempts,
        .sweep_interval_ms = static_cast<std::int64_t>(cfg.events_dead_letter_sweep_ms)
    };
    events_runtime.addProjector(std::make_unique<dcn::feed::FeedProjector>(
        events_runtime.projectionStore(), feed_runtime.feed(), events_runtime.writeStrand(), projector_retry));
    events_runtime.addProjector(std::make_unique<dcn::registry::RegistryProjector>(
        events_runtime.projectionStore(),
        registry,
        events_runtime.writeStrand(),
        projector_retry));

    events_runtime.start();
    artifact_runtime.start();
    feed_runtime.start();
    spdlog::info(
        "Events runtime started (ingestion_enabled={}, local_source={}, chain_id={}, hot_db='{}')",
        events_runtime.ingestionEnabled(),
        cfg.chain_ingestion.use_local_evm_source,
        cfg.events_chain_id,
        cfg.events_db.string());
    spdlog::info(
        "Feed runtime started (feed_db='{}')",
        cfg.feed_db.string());

    if(!cfg.chain_ingestion.enabled)
    {
        spdlog::warn(
            "Event ingestion pipeline is DISABLED (no --chain-local-source configured). "
            "POST /connector, POST /transformation, and POST /condition still deploy to the EVM "
            "and return 201 with locally-computed fields, but registry materialization will NOT "
            "occur — the GET read endpoints will not reflect the deployed entities. "
            "Pass --chain-local-source to enable the local EVM source and close the deploy loop.");
    }

    const bool wal_enabled = !registry_db_in_memory;
    std::atomic<bool> wal_sync_worker_stopped = true;
    std::optional<dcn::storage::sqlite::WalSyncWorker> wal_sync_worker;
    if(wal_enabled)
    {
        // create wal sync worker
        wal_sync_worker_stopped.store(false, std::memory_order_release);
        wal_sync_worker.emplace(io_context, registry,  registry_wal_sync_interval);
        
        // start wal sync worker
        asio::co_spawn(
            io_context,
            wal_sync_worker->run(),
            [&wal_sync_worker_stopped](std::exception_ptr exception_ptr)
            {
                wal_sync_worker_stopped.store(true, std::memory_order_release);
                dcn::utils::logException(exception_ptr, "Registry WAL sync worker failed");
            }
        );
        spdlog::info("Registry WAL sync worker enabled with interval {}ms", registry_wal_sync_interval.count());
    }
    else
    {
        spdlog::info("Registry WAL sync worker disabled for in-memory SQLite database");
    }

    dcn::async::SignalWorker signal_worker(
        io_context,
        _configureShutdownSignals(),

        // graceful shutdown
        [&server,
         &wal_sync_worker,
         &wal_sync_worker_stopped,
         &registry,
         &events_runtime,
         &feed_runtime,
         &artifact_runtime,
         wal_enabled]() -> asio::awaitable<void>
        {
            return _runGracefulShutdown(
                server,
                wal_sync_worker,
                wal_sync_worker_stopped,
                registry,
                wal_enabled,
                events_runtime,
                feed_runtime,
                artifact_runtime);
        },

        // immediate shutdown
        [&wal_sync_worker, &events_runtime, &feed_runtime, &artifact_runtime]()
        {
            _runImmediateShutdown(wal_sync_worker, events_runtime, feed_runtime, artifact_runtime);
        }
    );
    
    signal_worker.start();

    asio::co_spawn(
        io_context,
        _runStartupAndListen(
            registry,
            evm,
            server,
            cfg),
        [&io_context](std::exception_ptr exception_ptr)
        {
            if(!exception_ptr)
            {
                return;
            }

            dcn::utils::logException(exception_ptr, "Startup/listen coroutine failed");
            spdlog::critical("Stopping io_context due to startup/listen failure");
            io_context.stop();
        });

    try
    {
        io_context.run();
    }catch(std::exception & e)
    {
        spdlog::error("Error: {}", e.what());
    }
    catch(...)
    {
        spdlog::error("Unknown error");
    }
    

    spdlog::debug("Program finished");
    return 0;
}
