#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>

#include <spdlog/spdlog.h>

#include "chain.hpp"
#include "publication.hpp"
#include "publisher.hpp"

#include "bootstrap.hpp"

namespace dcn::publish
{
    namespace
    {
        constexpr std::size_t MAX_CREATION_CODE_BYTES = 256 * 1024;

        std::expected<std::vector<std::uint8_t>, std::string> readCreationCode(
            const std::filesystem::path & path)
        {
            std::error_code ec;
            if(!std::filesystem::is_regular_file(path, ec) || ec)
            {
                return std::unexpected(std::format("missing compiled contract '{}'", path.string()));
            }

            std::ifstream input(path);
            if(!input.is_open())
            {
                return std::unexpected(std::format("cannot read '{}'", path.string()));
            }

            std::ostringstream buffer;
            buffer << input.rdbuf();
            std::string hex = buffer.str();
            std::erase_if(hex, [](const unsigned char c) { return std::isspace(c) != 0; });

            const auto bytes = evmc::from_hex(hex);
            if(!bytes || bytes->empty())
            {
                return std::unexpected(std::format("'{}' is not hex bytecode", path.string()));
            }
            if(bytes->size() > MAX_CREATION_CODE_BYTES)
            {
                return std::unexpected(std::format("'{}' exceeds the creation code limit", path.string()));
            }

            return std::vector<std::uint8_t>(bytes->begin(), bytes->end());
        }

        std::vector<std::uint8_t> withAddressArgs(
            std::vector<std::uint8_t> creation_code,
            const std::vector<chain::Address> & arguments)
        {
            std::vector<pt::abi::Component> components;
            components.reserve(arguments.size());
            for(const chain::Address & argument : arguments)
            {
                components.push_back(pt::abi::addressComponent(argument));
            }

            const std::vector<std::uint8_t> encoded = pt::abi::encodeTuple(components);
            creation_code.insert(creation_code.end(), encoded.begin(), encoded.end());
            return creation_code;
        }

        std::expected<chain::Address, std::string> deployedAddress(const rpc::TransactionReceipt & receipt)
        {
            const auto address = evmc::from_hex<chain::Address>(receipt.contract_address);
            if(!address || *address == chain::Address{})
            {
                return std::unexpected("deployment receipt carries no contract address");
            }
            return *address;
        }

        // The provider already holds this exact signed transaction, e.g. from an attempt
        // whose response was lost. The transaction hash is the same either way.
        bool isAlreadyKnown(const rpc::RpcError & error)
        {
            std::string message = error.message;
            std::ranges::transform(message, message.begin(), [](const unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return message.find("already known") != std::string::npos
                || message.find("known transaction") != std::string::npos
                || message.find("already imported") != std::string::npos;
        }

        std::int64_t steadyNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Sends contract creations one after another from the operator account. The nonce
        // is read once and then advanced locally, so consecutive deployments never depend
        // on a provider's pending pool having caught up.
        class Deployer
        {
            public:
                Deployer(
                    const rpc::EthClient & client,
                    const Signer & signer,
                    const std::uint64_t chain_id,
                    const BootstrapConfig & config)
                    : _client(client)
                    , _signer(signer)
                    , _chain_id(chain_id)
                    , _config(config)
                    , _from(chain::addressToHex(signer.address()))
                {
                }

                asio::awaitable<std::expected<rpc::TransactionReceipt, std::string>> deploy(
                    std::vector<std::uint8_t> creation_code)
                {
                    const std::string data_hex = chain::bytesToHex(creation_code);

                    const auto estimate = co_await _client.estimateGas(_from, std::nullopt, data_hex);
                    if(!estimate)
                    {
                        if(estimate.error().kind == rpc::RpcError::Kind::EXECUTION)
                        {
                            co_return std::unexpected(std::format(
                                "deployment would revert: {}",
                                estimate.error().data.empty()
                                    ? estimate.error().message
                                    : describeRevert(estimate.error().data)));
                        }
                        co_return std::unexpected(std::format("eth_estimateGas failed: {}", estimate.error().message));
                    }
                    if(*estimate > _config.max_gas)
                    {
                        co_return std::unexpected(std::format(
                            "gas estimate {} exceeds the configured maximum {}", *estimate, _config.max_gas));
                    }

                    const auto fees = co_await _client.feeEstimate();
                    if(!fees)
                    {
                        co_return std::unexpected(std::format("fee lookup failed: {}", fees.error().message));
                    }
                    if(fees->max_fee_per_gas > _config.max_fee_per_gas_wei)
                    {
                        co_return std::unexpected(std::format(
                            "network fee {} wei/gas is above the configured ceiling {} wei/gas",
                            fees->max_fee_per_gas,
                            _config.max_fee_per_gas_wei));
                    }

                    if(!_next_nonce)
                    {
                        const auto count = co_await _client.transactionCount(_from, rpc::BLOCK_TAG_PENDING);
                        if(!count)
                        {
                            co_return std::unexpected(std::format(
                                "eth_getTransactionCount failed: {}", count.error().message));
                        }
                        _next_nonce = *count;
                    }

                    const Transaction transaction{
                        .chain_id = _chain_id,
                        .nonce = *_next_nonce,
                        .max_priority_fee_per_gas = fees->max_priority_fee_per_gas,
                        .max_fee_per_gas = fees->max_fee_per_gas,
                        .gas_limit = std::min(
                            _config.max_gas,
                            *estimate + (*estimate * _config.gas_limit_headroom_percent) / 100),
                        .to = std::nullopt,
                        .value = 0,
                        .data = std::move(creation_code)
                    };

                    const auto raw = _signer.signTransaction(transaction);
                    if(!raw)
                    {
                        co_return std::unexpected(raw.error());
                    }
                    const std::string tx_hash = transactionHash(*raw);

                    const auto sent = co_await _client.sendRawTransaction(*raw);
                    if(!sent && !isAlreadyKnown(sent.error()))
                    {
                        // The nonce may have been consumed elsewhere; read it again next time.
                        _next_nonce.reset();
                        co_return std::unexpected(std::format(
                            "eth_sendRawTransaction failed: {}", sent.error().message));
                    }
                    ++*_next_nonce;

                    const auto receipt = co_await _awaitReceipt(tx_hash);
                    if(!receipt) co_return std::unexpected(receipt.error());
                    if(!receipt->success)
                    {
                        co_return std::unexpected(std::format("deployment transaction {} reverted", tx_hash));
                    }
                    co_return *receipt;
                }

            private:
                asio::awaitable<std::expected<rpc::TransactionReceipt, std::string>> _awaitReceipt(
                    const std::string & tx_hash) const
                {
                    const std::int64_t deadline = steadyNowMs() + static_cast<std::int64_t>(_config.receipt_deadline_ms);
                    asio::steady_timer timer(co_await asio::this_coro::executor);

                    while(true)
                    {
                        const auto receipt = co_await _client.transactionReceipt(tx_hash);
                        if(receipt && receipt->has_value())
                        {
                            co_return **receipt;
                        }
                        if(!receipt && !receipt.error().retryable())
                        {
                            co_return std::unexpected(std::format(
                                "receipt lookup for {} failed: {}", tx_hash, receipt.error().message));
                        }
                        if(steadyNowMs() >= deadline)
                        {
                            co_return std::unexpected(std::format(
                                "no receipt for {} before the deadline; the deployment outcome is unknown", tx_hash));
                        }

                        timer.expires_after(std::chrono::milliseconds(_config.receipt_poll_interval_ms));
                        std::error_code wait_ec;
                        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));
                    }
                }

                const rpc::EthClient & _client;
                const Signer & _signer;
                std::uint64_t _chain_id;
                BootstrapConfig _config;
                std::string _from;
                std::optional<std::uint64_t> _next_nonce;
        };
    }

    asio::awaitable<std::expected<DeploymentManifest, std::string>> bootstrapDeployment(
        const rpc::EthClient & client,
        const Signer & signer,
        const BootstrapArtifacts & artifacts,
        const std::uint64_t chain_id,
        std::string solc_version,
        std::string pt_revision,
        BootstrapConfig config)
    {
        const auto registry_impl_code = readCreationCode(artifacts.registry_impl_bin);
        if(!registry_impl_code) co_return std::unexpected(registry_impl_code.error());

        const auto registry_proxy_code = readCreationCode(artifacts.registry_proxy_bin);
        if(!registry_proxy_code) co_return std::unexpected(registry_proxy_code.error());

        const auto runner_impl_code = readCreationCode(artifacts.runner_impl_bin);
        if(!runner_impl_code) co_return std::unexpected(runner_impl_code.error());

        const auto runner_proxy_code = readCreationCode(artifacts.runner_proxy_bin);
        if(!runner_proxy_code) co_return std::unexpected(runner_proxy_code.error());

        Deployer deployer(client, signer, chain_id, config);

        spdlog::info("Deploying PT registry implementation...");
        const auto registry_impl_receipt = co_await deployer.deploy(*registry_impl_code);
        if(!registry_impl_receipt) co_return std::unexpected(registry_impl_receipt.error());
        const auto registry_impl = deployedAddress(*registry_impl_receipt);
        if(!registry_impl) co_return std::unexpected(registry_impl.error());

        spdlog::info("Deploying PT registry proxy...");
        const auto registry_proxy_receipt =
            co_await deployer.deploy(withAddressArgs(*registry_proxy_code, {*registry_impl}));
        if(!registry_proxy_receipt) co_return std::unexpected(registry_proxy_receipt.error());
        const auto registry_proxy = deployedAddress(*registry_proxy_receipt);
        if(!registry_proxy) co_return std::unexpected(registry_proxy.error());

        spdlog::info("Deploying PT runner implementation...");
        const auto runner_impl_receipt = co_await deployer.deploy(*runner_impl_code);
        if(!runner_impl_receipt) co_return std::unexpected(runner_impl_receipt.error());
        const auto runner_impl = deployedAddress(*runner_impl_receipt);
        if(!runner_impl) co_return std::unexpected(runner_impl.error());

        spdlog::info("Deploying PT runner proxy...");
        const auto runner_proxy_receipt =
            co_await deployer.deploy(withAddressArgs(*runner_proxy_code, {*runner_impl, *registry_proxy}));
        if(!runner_proxy_receipt) co_return std::unexpected(runner_proxy_receipt.error());
        const auto runner_proxy = deployedAddress(*runner_proxy_receipt);
        if(!runner_proxy) co_return std::unexpected(runner_proxy.error());

        const auto registry_impl_hash = co_await rpc::readCodeHash(client, chain::addressToHex(*registry_impl));
        if(!registry_impl_hash || !registry_impl_hash->has_value())
        {
            co_return std::unexpected("deployed registry implementation has no code");
        }

        const auto runner_impl_hash = co_await rpc::readCodeHash(client, chain::addressToHex(*runner_impl));
        if(!runner_impl_hash || !runner_impl_hash->has_value())
        {
            co_return std::unexpected("deployed runner implementation has no code");
        }

        DeploymentManifest manifest{
            .chain_id = chain_id,
            .registry = chain::addressToHex(*registry_proxy),
            .runner = chain::addressToHex(*runner_proxy),
            .registry_impl = chain::addressToHex(*registry_impl),
            .runner_impl = chain::addressToHex(*runner_impl),
            .registry_impl_code_hash = **registry_impl_hash,
            .runner_impl_code_hash = **runner_impl_hash,
            .registry_tx = registry_proxy_receipt->tx_hash,
            .runner_tx = runner_proxy_receipt->tx_hash,
            // Events can only exist from the block the registry proxy was created in.
            .deployment_block = registry_proxy_receipt->block_number,
            .protocol_id = rpc::ptProtocolId(),
            .protocol_version = rpc::PT_PROTOCOL_VERSION,
            .solc_version = std::move(solc_version),
            .pt_revision = std::move(pt_revision),
            .owner = chain::addressToHex(signer.address())
        };

        // The manifest is only worth persisting if the deployment it describes verifies.
        const auto verified = co_await verifyDeployment(client, manifest);
        if(!verified)
        {
            co_return std::unexpected(std::format(
                "bootstrapped deployment failed verification: {}", verified.error()));
        }

        spdlog::info(
            "PT deployed: registry {} runner {} at block {}",
            manifest.registry,
            manifest.runner,
            manifest.deployment_block);

        co_return manifest;
    }
}
