#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include <asio.hpp>

#include "pt_reads.hpp"

namespace dcn::publish
{
    /// Everything needed to positively identify one canonical PT deployment. A fresh
    /// server cannot discover a canonical registry by scanning a chain, so publication
    /// always works from a manifest: the reviewed one committed with a PT release, or
    /// the one this server wrote when it bootstrapped a private deployment.
    struct DeploymentManifest
    {
        std::uint64_t chain_id = 0;
        std::string registry;
        std::string runner;
        std::string registry_impl;
        std::string runner_impl;
        std::string registry_impl_code_hash;
        std::string runner_impl_code_hash;
        std::string registry_tx;
        std::string runner_tx;
        std::int64_t deployment_block = 0;
        std::string protocol_id;
        std::uint64_t protocol_version = 0;
        std::string solc_version;
        std::string pt_revision;
        std::string owner;
    };

    std::expected<DeploymentManifest, std::string> readManifest(const std::filesystem::path & path);

    /// Written through a temporary file and renamed, so a crash mid-write cannot leave
    /// a half-manifest that a later startup would treat as a valid deployment.
    std::expected<void, std::string> writeManifest(
        const std::filesystem::path & path,
        const DeploymentManifest & manifest);

    /// Confirms the live chain still matches the manifest before it is used: chain id,
    /// proxy implementation slots, implementation code hashes, protocol identity/version,
    /// the runner-to-registry link and the pinned upgrade owner of both proxies.
    asio::awaitable<std::expected<void, std::string>> verifyDeployment(
        const rpc::EthClient & client,
        const DeploymentManifest & manifest);
}
