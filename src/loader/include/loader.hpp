#pragma once

#include <cstddef>
#include <fstream>

#include "native.h"
#include <asio.hpp>
#include <spdlog/spdlog.h>

#include "pt.hpp"
#include "registry.hpp"
#include "parser.hpp"
#include "evm.hpp"
#include "chain.hpp"

namespace dcn::loader
{
    bool ensurePTBuildVersion(const std::filesystem::path & storage_path);

    /// Identifies the complete PT Solidity source tree the server deploys from. Recorded
    /// in the deployment manifest; an artifact records only the sources it compiled.
    std::string ptSourceRevision(const std::filesystem::path & pt_path);

    /// A compiled entity bound to one publication: the exact artifact bytes whose hash
    /// the publication carries, and the creation code the registry factory deploys with
    /// CREATE2.
    struct PreparedPublication
    {
        pt::Publication publication;
        std::vector<std::uint8_t> creation_code;
        std::string artifact_bytes;
        std::string content_hash;
        std::optional<pt::ConnectorRegistration> registration;   // connectors only
        std::vector<pt::ArtifactDependency> dependencies;         // connectors only
    };

    /// The definition an entity was created with on this server, including its Solidity
    /// source, as retained in local storage. nullopt for an unknown or invalid name.
    std::optional<TransformationRecord> loadLocalTransformation(
        const std::filesystem::path & storage_path,
        const std::string & name);
    std::optional<ConditionRecord> loadLocalCondition(
        const std::filesystem::path & storage_path,
        const std::string & name);
    std::optional<ConnectorRecord> loadLocalConnector(
        const std::filesystem::path & storage_path,
        const std::string & name);

    /// Rebuilds the publication for a stored entity without deploying it, so it can be
    /// published to a registry on another chain. `target_registry` is the registry the
    /// entity will be registered in: it becomes a connector constructor argument, so a
    /// connector's creation code differs per target while its artifact bytes do not.
    asio::awaitable<std::expected<PreparedPublication, pt::PTDeployError>> prepareTransformationPublication(
        evm::EVM & evm,
        TransformationRecord transformation,
        const std::filesystem::path & storage_path,
        std::string expected_content_hash = {});
    asio::awaitable<std::expected<PreparedPublication, pt::PTDeployError>> prepareConditionPublication(
        evm::EVM & evm,
        ConditionRecord condition,
        const std::filesystem::path & storage_path,
        std::string expected_content_hash = {});
    asio::awaitable<std::expected<PreparedPublication, pt::PTDeployError>> prepareConnectorPublication(
        evm::EVM & evm,
        ConnectorRecord connector,
        const std::filesystem::path & storage_path,
        const chain::Address & target_registry,
        std::string expected_content_hash = {});

    asio::awaitable<std::expected<chain::Address, pt::PTDeployError>> deployConnector(
        evm::EVM & evm,
        registry::Registry & registry,
        ConnectorRecord connector,
        const std::filesystem::path & storage_path,
        bool persist_json = true);
    asio::awaitable<std::expected<chain::Address, pt::PTDeployError>> deployTransformation(
        evm::EVM & evm,
        registry::Registry & registry,
        TransformationRecord transformation,
        const std::filesystem::path & storage_path,
        bool persist_json = true,
        std::string expected_content_hash = {});
    asio::awaitable<std::expected<chain::Address, pt::PTDeployError>> deployCondition(
        evm::EVM & evm,
        registry::Registry & registry,
        ConditionRecord condition,
        const std::filesystem::path & storage_path,
        bool persist_json = true,
        std::string expected_content_hash = {});

    asio::awaitable<bool> importJsonStorageToDatabase(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::filesystem::path & storage_path);

    asio::awaitable<bool> ensureTransformationDeployed(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::string & name,
        const std::filesystem::path & storage_path);
    asio::awaitable<bool> ensureConditionDeployed(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::string & name,
        const std::filesystem::path & storage_path);
    asio::awaitable<bool> ensureConnectorDeployed(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::string & name,
        const std::filesystem::path & storage_path);


    asio::awaitable<bool> loadStoredConnectors(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::filesystem::path & storage_path);
    asio::awaitable<bool> loadStoredConditions(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::filesystem::path & storage_path);
    asio::awaitable<bool> loadStoredTransformations(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::filesystem::path & storage_path);
}
