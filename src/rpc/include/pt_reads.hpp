#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio.hpp>

#include "eth_client.hpp"
#include "publication.hpp"

namespace dcn::pt
{
    enum class EntityKind : std::uint8_t;
}

namespace dcn::rpc
{
    // PT protocol reads over eth_call against a registry or runner proxy.

    // The creation code of every publish call in the calldata of `tx_hash`, which emitted
    // its registry events in the block with `block_hash`, keyed by the address `registry`
    // deployed it to (see pt::findPublishedCreationCodes). Empty when the calldata holds
    // none (a relay that builds the call on chain). An unknown transaction, or one mined in
    // another block, is an error: the event names it.
    asio::awaitable<std::expected<std::map<chain::Address, std::vector<std::uint8_t>>, RpcError>> readPublishedCreationCodes(
        const EthClient & client,
        std::string_view tx_hash,
        std::string_view block_hash,
        const chain::Address & registry);

    // The transaction and block of the registry event that published the transformation or
    // condition `name` at `entity_address`, no later than `latest_block`. The entity's code
    // first appears in the block it was published in, which a binary search over historical
    // eth_getCode finds; that block's logs then name the transaction. Needs a node serving
    // historical state. nullopt when no such publication is found.
    struct PublicationLocation
    {
        std::string tx_hash;
        std::string block_hash;
    };
    asio::awaitable<std::expected<std::optional<PublicationLocation>, RpcError>> findEntityPublication(
        const EthClient & client,
        std::string_view registry_address,
        pt::EntityKind kind,
        std::string_view name,
        const chain::Address & entity_address,
        std::int64_t latest_block);

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

    // Positively identifies a configured runner proxy by its PT protocol identity and
    // version. A runner serves any registry passed to Runner.gen, so it carries no link to
    // check. Returns a human-readable reason on rejection.
    asio::awaitable<std::expected<void, std::string>> verifyPtRunner(
        const EthClient & client,
        std::string_view runner_address);

    // OwnableBase `getOwner()` of the registry or runner proxy.
    asio::awaitable<std::expected<std::string, RpcError>> readOwner(
        const EthClient & client,
        std::string_view contract_address);

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
