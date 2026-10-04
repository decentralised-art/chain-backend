#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <asio.hpp>

#include "pt_reads.hpp"

namespace dcn::publish
{
    /// The two upgradeable PT contracts a deployment consists of.
    enum class PtContract
    {
        REGISTRY,
        RUNNER
    };

    /// "registry" or "runner"; also the prefix of the contract's manifest fields.
    std::string_view contractLabel(PtContract contract);

    /// Everything needed to positively identify one canonical PT deployment. A fresh
    /// server cannot discover a canonical registry by scanning a chain, so publication
    /// always works from a manifest: the reviewed one committed with a PT release, or
    /// the one this server wrote when it deployed a private deployment. A registry is
    /// always present. The runner serves any registry, so it is optional: its fields are
    /// either all set or all empty.
    struct DeploymentManifest
    {
        std::uint64_t chain_id = 0;
        std::string registry{};
        std::string runner{};
        std::string registry_impl{};
        std::string runner_impl{};
        std::string registry_impl_code_hash{};
        std::string runner_impl_code_hash{};
        std::string registry_tx{};
        std::string runner_tx{};
        std::int64_t deployment_block = 0;
        std::string protocol_id{};
        std::uint64_t protocol_version = 0;
        std::string solc_version{};
        std::string pt_revision{};
        std::string owner{};
    };

    std::expected<DeploymentManifest, std::string> readManifest(const std::filesystem::path & path);

    /// The manifest's JSON text, as writeManifest stores it.
    std::string formatManifest(const DeploymentManifest & manifest);

    /// Written through a temporary file and renamed, so a crash mid-write cannot leave
    /// a half-manifest that a later startup would treat as a valid deployment.
    std::expected<void, std::string> writeManifest(
        const std::filesystem::path & path,
        const DeploymentManifest & manifest);

    /// Verifies one contract of the manifest: protocol identity (and chain id for the
    /// registry), proxy implementation slot, implementation code hash and upgrade owner.
    asio::awaitable<std::expected<void, std::string>> verifyContract(
        const rpc::EthClient & client,
        const DeploymentManifest & manifest,
        PtContract contract);

    /// Confirms the live chain still matches the manifest before it is used: the
    /// registry, and the runner when the manifest has one.
    asio::awaitable<std::expected<void, std::string>> verifyDeployment(
        const rpc::EthClient & client,
        const DeploymentManifest & manifest);
}
