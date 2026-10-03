#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include <asio.hpp>

#include "eth_client.hpp"
#include "manifest.hpp"
#include "signer.hpp"

namespace dcn::publish
{
    struct DeployConfig
    {
        // A provider's gas estimate is advisory; it is never authority to spend without a
        // limit of our own.
        std::uint64_t max_gas = 12'000'000;
        std::uint64_t max_fee_per_gas_wei = 200'000'000'000;   // 200 gwei
        std::uint64_t gas_limit_headroom_percent = 20;

        std::uint64_t receipt_deadline_ms = 120'000;
        std::uint64_t receipt_poll_interval_ms = 2'000;
    };

    /// Deploys an implementation of `contract` and a proxy that initializes it in its
    /// constructor, one transaction at a time, and returns `manifest` with that contract
    /// replaced by the new one and the signer as owner. The other contract is kept: it
    /// must verify against the manifest before anything is sent, and the signer must be
    /// its owner, since both proxies share one pinned upgrade owner. Deploying a registry
    /// also sets the deployment block and protocol identity; a runner can only be added
    /// to a manifest that has a registry. `manifest.chain_id` names the chain to sign for.
    /// The result is verified on chain before it is returned.
    asio::awaitable<std::expected<DeploymentManifest, std::string>> deployContract(
        const rpc::EthClient & client,
        const Signer & signer,
        PtContract contract,
        const std::filesystem::path & impl_bin,
        const std::filesystem::path & proxy_bin,
        DeploymentManifest manifest,
        DeployConfig config = {});

    /// Deploys an implementation of `contract` and points the manifest's proxy of it at
    /// that implementation with upgradeToAndCall, so its address stays the same. The live
    /// deployment must verify against the manifest before anything is sent, and the signer
    /// must be its upgrade owner. Returns the manifest pinning the new implementation. The
    /// new implementation must keep the storage layout of the one it replaces.
    /// Errors after submitting the upgrade carry the implementation and code hash needed
    /// to recover the manifest once the proxy's actual implementation is checked.
    asio::awaitable<std::expected<DeploymentManifest, std::string>> upgradeContract(
        const rpc::EthClient & client,
        const Signer & signer,
        PtContract contract,
        const std::filesystem::path & impl_bin,
        DeploymentManifest manifest,
        DeployConfig config = {});
}
