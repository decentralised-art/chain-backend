#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <asio.hpp>

#include "eth_client.hpp"

namespace dcn::pt
{
    enum class EntityKind : std::uint8_t;
}

namespace dcn::rpc
{
    // PT protocol reads over eth_call against a registry or runner proxy.

    struct PtIdentity
    {
        std::string protocol_id;
        std::uint64_t protocol_version = 0;
    };

    // keccak256("hypermusic.pt"), as declared by RegistryBase and Runner.
    std::string ptProtocolId();
    constexpr std::uint64_t PT_PROTOCOL_VERSION = 1;

    asio::awaitable<std::expected<PtIdentity, RpcError>> readPtIdentity(
        const EthClient & client,
        std::string_view contract_address);

    // Positively identifies a configured proxy address as a compatible PT registry:
    // chain id, deployed code, protocol id and protocol version must all agree.
    // Returns a human-readable reason on rejection.
    asio::awaitable<std::expected<PtIdentity, std::string>> verifyPtRegistry(
        const EthClient & client,
        std::uint64_t expected_chain_id,
        std::string_view registry_address);

    // Positively identifies a configured runner proxy: PT protocol identity and version,
    // and a registry() link to `registry_address`. Returns a human-readable reason on
    // rejection.
    asio::awaitable<std::expected<void, std::string>> verifyPtRunner(
        const EthClient & client,
        std::string_view runner_address,
        std::string_view registry_address);

    // OwnableBase `getOwner()` of the registry or runner proxy.
    asio::awaitable<std::expected<std::string, RpcError>> readOwner(
        const EthClient & client,
        std::string_view contract_address);

    // `Runner.registry()`.
    asio::awaitable<std::expected<std::string, RpcError>> readRunnerRegistry(
        const EthClient & client,
        std::string_view runner_address);

    // `publicationNonce(owner)`.
    asio::awaitable<std::expected<std::uint64_t, RpcError>> readPublicationNonce(
        const EthClient & client,
        std::string_view registry_address,
        std::string_view owner_address);

    // `contains{Connector,Transformation,Condition}(name)`; PT keeps a separate name
    // space per entity kind.
    asio::awaitable<std::expected<bool, RpcError>> registryContainsEntity(
        const EthClient & client,
        std::string_view registry_address,
        pt::EntityKind kind,
        std::string_view name,
        std::string_view tag = BLOCK_TAG_LATEST);

    // `getEntityContentHash(kind, name)`; the zero hash when the name is not registered.
    asio::awaitable<std::expected<std::string, RpcError>> readEntityContentHash(
        const EthClient & client,
        std::string_view registry_address,
        pt::EntityKind kind,
        std::string_view name);

    // `getEntityOwner(kind, name)`; the zero address when the name is not registered.
    asio::awaitable<std::expected<std::string, RpcError>> readEntityOwner(
        const EthClient & client,
        std::string_view registry_address,
        pt::EntityKind kind,
        std::string_view name);

    // `get{Connector,Transformation,Condition}(name)`; reverts when the name is missing.
    asio::awaitable<std::expected<std::string, RpcError>> readEntityAddress(
        const EthClient & client,
        std::string_view registry_address,
        pt::EntityKind kind,
        std::string_view name);
}
