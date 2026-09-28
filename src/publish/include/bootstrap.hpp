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
    /// Compiler output for the four contracts a deployment consists of.
    struct BootstrapArtifacts
    {
        std::filesystem::path registry_impl_bin;
        std::filesystem::path registry_proxy_bin;
        std::filesystem::path runner_impl_bin;
        std::filesystem::path runner_proxy_bin;
    };

    struct BootstrapConfig
    {
        // A provider's gas estimate is advisory; it is never authority to spend without a
        // limit of our own.
        std::uint64_t max_gas = 12'000'000;
        std::uint64_t max_fee_per_gas_wei = 200'000'000'000;   // 200 gwei
        std::uint64_t gas_limit_headroom_percent = 20;

        std::uint64_t receipt_deadline_ms = 120'000;
        std::uint64_t receipt_poll_interval_ms = 2'000;
    };

    /// Deploys registry and runner implementations and their proxies with the operator
    /// key, one transaction at a time, waits for every receipt, verifies the result and
    /// returns the manifest describing it. Each proxy initializes itself in its
    /// constructor, so the operator account becomes the upgrade owner of both.
    asio::awaitable<std::expected<DeploymentManifest, std::string>> bootstrapDeployment(
        const rpc::EthClient & client,
        const Signer & signer,
        const BootstrapArtifacts & artifacts,
        std::uint64_t chain_id,
        std::string solc_version,
        std::string pt_revision,
        BootstrapConfig config = {});
}
