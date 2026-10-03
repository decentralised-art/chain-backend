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

    /// A compiled entity bound to one publication: the creation code the registry factory
    /// deploys with CREATE2, and the bundle bytes whose hash is the publication's content
    /// hash. The bundle is only hashed, never stored.
    struct PreparedPublication
    {
        pt::Publication publication;
        std::vector<std::uint8_t> creation_code;
        // Code the entity runs once deployed; its hash is publication.runtime_code_hash.
        std::vector<std::uint8_t> runtime_code;
        std::string artifact_bytes;
        std::string content_hash;   // publication.content_hash as hex
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
    /// connector's creation code differs per target while its content hash does not.
    asio::awaitable<std::expected<PreparedPublication, pt::PTDeployError>> prepareTransformationPublication(
        evm::EVM & evm,
        TransformationRecord transformation,
        const std::filesystem::path & storage_path);
    asio::awaitable<std::expected<PreparedPublication, pt::PTDeployError>> prepareConditionPublication(
        evm::EVM & evm,
        ConditionRecord condition,
        const std::filesystem::path & storage_path);
    asio::awaitable<std::expected<PreparedPublication, pt::PTDeployError>> prepareConnectorPublication(
        evm::EVM & evm,
        ConnectorRecord connector,
        const std::filesystem::path & storage_path,
        const chain::Address & target_registry);

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
        bool persist_json = true);
    asio::awaitable<std::expected<chain::Address, pt::PTDeployError>> deployCondition(
        evm::EVM & evm,
        registry::Registry & registry,
        ConditionRecord condition,
        const std::filesystem::path & storage_path,
        bool persist_json = true);

    asio::awaitable<bool> importJsonStorageToDatabase(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::filesystem::path & storage_path);

    /// Deploys the entity on the local EVM for runtime use, together with a connector's
    /// dependencies. A local entity is built from its retained definition. A chain entity is
    /// replayed from the creation code the chain deployed it from, as projected into the
    /// registry, so its code and constructor state are the chain's; no compiler is involved
    /// and the chain is not contacted. On failure, `failure` receives the first reason a
    /// chain entity could not be deployed, naming the entity.
    asio::awaitable<bool> ensureTransformationDeployed(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::string & name,
        const std::filesystem::path & storage_path,
        std::string * failure = nullptr);
    asio::awaitable<bool> ensureConditionDeployed(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::string & name,
        const std::filesystem::path & storage_path,
        std::string * failure = nullptr);
    asio::awaitable<bool> ensureConnectorDeployed(
        evm::EVM & evm,
        registry::Registry & registry,
        const std::string & name,
        const std::filesystem::path & storage_path,
        std::string * failure = nullptr);


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
