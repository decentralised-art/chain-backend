#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
#include <sstream>

#include <spdlog/spdlog.h>

#include "chain.hpp"
#include "publication.hpp"
#include "publisher.hpp"

#include "deploy.hpp"

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

        // Creation code followed by one ABI-encoded address constructor argument.
        std::vector<std::uint8_t> withAddressArg(std::vector<std::uint8_t> creation_code, const chain::Address & argument)
        {
            const std::vector<std::uint8_t> encoded = pt::abi::encodeTuple({pt::abi::addressComponent(argument)});
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

        std::int64_t steadyNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Sends transactions one after another from the operator account. The nonce is read
        // once and then advanced locally, so consecutive transactions never depend on a
        // provider's pending pool having caught up.
        class Deployer
        {
            public:
                Deployer(
                    const rpc::EthClient & client,
                    const Signer & signer,
                    const std::uint64_t chain_id,
                    const DeployConfig & config)
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
                    co_return co_await _send(std::nullopt, std::move(creation_code));
                }

                asio::awaitable<std::expected<rpc::TransactionReceipt, std::string>> call(
                    const chain::Address to,
                    std::vector<std::uint8_t> data)
                {
                    co_return co_await _send(to, std::move(data));
                }

            private:
                asio::awaitable<std::expected<rpc::TransactionReceipt, std::string>> _send(
                    const std::optional<chain::Address> to,
                    std::vector<std::uint8_t> data)
                {
                    const std::string data_hex = chain::bytesToHex(data);
                    const std::optional<std::string> to_hex = to
                        ? std::optional<std::string>(chain::addressToHex(*to))
                        : std::nullopt;

                    const auto estimate = co_await _client.estimateGas(
                        _from,
                        to_hex ? std::optional<std::string_view>(*to_hex) : std::nullopt,
                        data_hex);
                    if(!estimate)
                    {
                        if(estimate.error().kind == rpc::RpcError::Kind::EXECUTION)
                        {
                            co_return std::unexpected(std::format(
                                "transaction would revert: {}",
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
                        .to = to,
                        .value = 0,
                        .data = std::move(data)
                    };

                    const auto raw = _signer.signTransaction(transaction);
                    if(!raw)
                    {
                        co_return std::unexpected(raw.error());
                    }
                    const std::string tx_hash = transactionHash(*raw);

                    const auto sent = co_await _client.sendRawTransaction(*raw);
                    if(!sent && !rpc::isAlreadyKnown(sent.error()))
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
                        co_return std::unexpected(std::format("transaction {} reverted", tx_hash));
                    }
                    co_return *receipt;
                }

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
                                "no receipt for {} before the deadline; the transaction outcome is unknown", tx_hash));
                        }

                        timer.expires_after(std::chrono::milliseconds(_config.receipt_poll_interval_ms));
                        std::error_code wait_ec;
                        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));
                    }
                }

                const rpc::EthClient & _client;
                const Signer & _signer;
                std::uint64_t _chain_id;
                DeployConfig _config;
                std::string _from;
                std::optional<std::uint64_t> _next_nonce;
        };

        asio::awaitable<std::expected<std::string, std::string>> implementationCodeHash(
            const rpc::EthClient & client,
            const chain::Address implementation,
            const std::string_view label)
        {
            const auto code_hash = co_await rpc::readCodeHash(client, chain::addressToHex(implementation));
            if(!code_hash || !code_hash->has_value())
            {
                co_return std::unexpected(std::format("deployed {} implementation has no code", label));
            }
            co_return **code_hash;
        }

        std::vector<std::uint8_t> upgradeToAndCallData(const chain::Address & implementation)
        {
            std::vector<std::uint8_t> data = chain::constructSelector("upgradeToAndCall(address,bytes)");
            const std::vector<std::uint8_t> arguments = pt::abi::encodeTuple({
                pt::abi::addressComponent(implementation),
                pt::abi::bytesComponent({})});
            data.insert(data.end(), arguments.begin(), arguments.end());
            return data;
        }

        // Both proxies of a deployment share one pinned upgrade owner, so only that
        // account may change a deployment that already has contracts.
        std::expected<void, std::string> requireOwner(const Signer & signer, const DeploymentManifest & manifest)
        {
            const std::string signer_address = chain::addressToHex(signer.address());
            if(signer_address != manifest.owner)
            {
                return std::unexpected(std::format(
                    "the signing account {} is not the deployment owner {}", signer_address, manifest.owner));
            }
            return {};
        }

        // The manifest fields describing one contract.
        struct ContractFields
        {
            std::string & proxy;
            std::string & impl;
            std::string & impl_code_hash;
            std::string & tx;
        };

        ContractFields fieldsOf(DeploymentManifest & manifest, const PtContract contract)
        {
            if(contract == PtContract::REGISTRY)
            {
                return {manifest.registry, manifest.registry_impl, manifest.registry_impl_code_hash, manifest.registry_tx};
            }
            return {manifest.runner, manifest.runner_impl, manifest.runner_impl_code_hash, manifest.runner_tx};
        }
    }

    asio::awaitable<std::expected<DeploymentManifest, std::string>> deployContract(
        const rpc::EthClient & client,
        const Signer & signer,
        const PtContract contract,
        const std::filesystem::path & impl_bin,
        const std::filesystem::path & proxy_bin,
        DeploymentManifest manifest,
        DeployConfig config)
    {
        const std::string_view label = contractLabel(contract);
        const PtContract other = contract == PtContract::REGISTRY ? PtContract::RUNNER : PtContract::REGISTRY;

        if(contract == PtContract::RUNNER && manifest.registry.empty())
        {
            co_return std::unexpected("a runner can only be added to a deployment that has a registry");
        }
        if(!fieldsOf(manifest, other).proxy.empty())
        {
            if(const auto owner = requireOwner(signer, manifest); !owner) co_return std::unexpected(owner.error());

            // The contract that stays must be the one the manifest describes before gas is
            // spent on a deployment around it.
            const auto current = co_await verifyContract(client, manifest, other);
            if(!current)
            {
                co_return std::unexpected(std::format(
                    "the current {} does not match the manifest: {}", contractLabel(other), current.error()));
            }
        }

        auto impl_code = readCreationCode(impl_bin);
        if(!impl_code) co_return std::unexpected(impl_code.error());

        auto proxy_code = readCreationCode(proxy_bin);
        if(!proxy_code) co_return std::unexpected(proxy_code.error());

        Deployer deployer(client, signer, manifest.chain_id, config);

        spdlog::info("Deploying PT {} implementation...", label);
        const auto impl_receipt = co_await deployer.deploy(std::move(*impl_code));
        if(!impl_receipt) co_return std::unexpected(impl_receipt.error());
        const auto impl = deployedAddress(*impl_receipt);
        if(!impl) co_return std::unexpected(impl.error());

        spdlog::info("Deploying PT {} proxy...", label);
        const auto proxy_receipt = co_await deployer.deploy(withAddressArg(std::move(*proxy_code), *impl));
        if(!proxy_receipt) co_return std::unexpected(proxy_receipt.error());
        const auto proxy = deployedAddress(*proxy_receipt);
        if(!proxy) co_return std::unexpected(proxy.error());

        const auto impl_hash = co_await implementationCodeHash(client, *impl, label);
        if(!impl_hash) co_return std::unexpected(impl_hash.error());

        const ContractFields fields = fieldsOf(manifest, contract);
        fields.proxy = chain::addressToHex(*proxy);
        fields.impl = chain::addressToHex(*impl);
        fields.impl_code_hash = *impl_hash;
        fields.tx = proxy_receipt->tx_hash;
        manifest.owner = chain::addressToHex(signer.address());
        if(contract == PtContract::REGISTRY)
        {
            // Events can only exist from the block the registry proxy was created in.
            manifest.deployment_block = proxy_receipt->block_number;
            manifest.protocol_id = rpc::ptProtocolId();
            manifest.protocol_version = rpc::PT_PROTOCOL_VERSION;
        }

        // The manifest is only worth persisting if the deployment it describes verifies.
        const auto verified = co_await verifyDeployment(client, manifest);
        if(!verified)
        {
            co_return std::unexpected(std::format(
                "{} {} (implementation {}) was deployed but failed verification: {}",
                label, fields.proxy, fields.impl, verified.error()));
        }

        spdlog::info("PT {} {} deployed, delegating to {}", label, fields.proxy, fields.impl);
        co_return manifest;
    }

    asio::awaitable<std::expected<DeploymentManifest, std::string>> upgradeContract(
        const rpc::EthClient & client,
        const Signer & signer,
        const PtContract contract,
        const std::filesystem::path & impl_bin,
        DeploymentManifest manifest,
        DeployConfig config)
    {
        const std::string_view label = contractLabel(contract);
        const ContractFields fields = fieldsOf(manifest, contract);
        if(fields.proxy.empty())
        {
            co_return std::unexpected(std::format("the manifest has no {} to upgrade", label));
        }
        if(const auto owner = requireOwner(signer, manifest); !owner) co_return std::unexpected(owner.error());
        const auto proxy = evmc::from_hex<chain::Address>(fields.proxy);
        if(!proxy)
        {
            co_return std::unexpected(std::format("the manifest {} address '{}' is invalid", label, fields.proxy));
        }

        auto impl_code = readCreationCode(impl_bin);
        if(!impl_code) co_return std::unexpected(impl_code.error());

        // The proxy about to be upgraded must be the one the manifest describes.
        const auto current = co_await verifyDeployment(client, manifest);
        if(!current)
        {
            co_return std::unexpected(std::format(
                "the current deployment does not match the manifest: {}", current.error()));
        }

        Deployer deployer(client, signer, manifest.chain_id, config);

        spdlog::info("Deploying PT {} implementation...", label);
        const auto impl_receipt = co_await deployer.deploy(std::move(*impl_code));
        if(!impl_receipt) co_return std::unexpected(impl_receipt.error());
        const auto impl = deployedAddress(*impl_receipt);
        if(!impl) co_return std::unexpected(impl.error());

        const auto impl_hash = co_await implementationCodeHash(client, *impl, label);
        if(!impl_hash) co_return std::unexpected(impl_hash.error());

        fields.impl = chain::addressToHex(*impl);
        fields.impl_code_hash = *impl_hash;

        // The proxy is only pointed at code that identifies as this PT protocol; protocolId()
        // and protocolVersion() are pure, so the implementation answers them directly.
        const auto identity = co_await rpc::readPtIdentity(client, fields.impl);
        if(!identity || identity->protocol_id != rpc::ptProtocolId()
           || identity->protocol_version != rpc::PT_PROTOCOL_VERSION)
        {
            co_return std::unexpected(std::format(
                "the new {} implementation {} does not identify as PT protocol version {}; the proxy was not upgraded",
                label, fields.impl, rpc::PT_PROTOCOL_VERSION));
        }

        const std::string recovery = std::format(
            "{} {}: verify the proxy implementation before retrying; if it is {}, "
            "recover the manifest by setting {}_impl={} and {}_impl_code_hash={}",
            label, fields.proxy, fields.impl, label, fields.impl, label, fields.impl_code_hash);

        spdlog::info("Upgrading PT {} proxy {}...", label, fields.proxy);
        const auto upgraded = co_await deployer.call(*proxy, upgradeToAndCallData(*impl));
        if(!upgraded) co_return std::unexpected(std::format("{}; {}", upgraded.error(), recovery));

        const auto verified = co_await verifyDeployment(client, manifest);
        if(!verified)
        {
            co_return std::unexpected(std::format(
                "upgrade transaction {} succeeded but verification failed: {}; {}",
                upgraded->tx_hash, verified.error(), recovery));
        }

        spdlog::info("PT {} {} now delegates to {}", label, fields.proxy, fields.impl);
        co_return manifest;
    }
}
