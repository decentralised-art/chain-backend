#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "chain.hpp"
#include "connector.pb.h"
#include "entity.hpp"

namespace dcn::pt
{
    /// The publication an owner authorizes. The registry deploys the entity with CREATE2
    /// and refuses to register unless the deployed runtime, its PT identity and these
    /// hashes all agree.
    struct Publication
    {
        EntityKind kind = EntityKind::TRANSFORMATION;
        std::string name;
        chain::Address owner{};
        evmc::bytes32 content_hash{};
        evmc::bytes32 metadata_hash{};
        evmc::bytes32 creation_code_hash{};
        evmc::bytes32 runtime_code_hash{};
        // computeRegistrationHash of the ConnectorAdded arrays for a connector, zero otherwise.
        evmc::bytes32 registration_hash{};
        std::uint64_t nonce = 0;
        std::uint64_t deadline = 0;
    };

    /// Minimal ABI writer covering the publication calldata shapes. Only the types the
    /// registry factory takes are supported.
    namespace abi
    {
        /// One encoded argument: a static 32-byte word, or a dynamic tail blob that the
        /// enclosing tuple replaces with an offset word.
        struct Component
        {
            bool dynamic = false;
            std::vector<std::uint8_t> data;
        };

        Component staticWord(std::vector<std::uint8_t> word);
        Component uintComponent(std::uint64_t value);
        Component addressComponent(const chain::Address & value);
        Component bytes32Component(const evmc::bytes32 & value);
        Component bytesComponent(std::span<const std::uint8_t> value);
        Component stringComponent(const std::string & value);
        Component uint32ArrayComponent(const std::vector<std::uint32_t> & values);
        Component int32ArrayComponent(const std::vector<std::int32_t> & values);
        Component stringArrayComponent(const std::vector<std::string> & values);

        /// head-then-tail encoding; dynamic components contribute an offset word measured
        /// from the start of the encoding.
        std::vector<std::uint8_t> encodeTuple(const std::vector<Component> & components);
    }

    /// The parallel arrays ConnectorAdded carries so a ConnectorRecord is reconstructable
    /// from chain logs alone. Built from the record the server is publishing.
    struct ConnectorRegistration
    {
        std::uint32_t dimensions_count = 0;
        std::vector<std::uint32_t> composite_dim_ids;
        std::vector<std::string> composite_names;
        std::vector<std::uint32_t> binding_dim_ids;
        std::vector<std::uint32_t> binding_slot_ids;
        std::vector<std::string> binding_names;
        std::string condition_name;
        std::vector<std::int32_t> condition_args;
        std::vector<std::uint32_t> static_ri_positions;
        std::vector<std::uint32_t> static_ri_start_points;
        std::vector<std::uint32_t> static_ri_transform_shifts;
        std::vector<std::uint32_t> transformation_dim_ids;
        std::vector<std::string> transformation_names;
        std::vector<std::uint32_t> transformation_arg_counts;
        std::vector<std::int32_t> transformation_args;

        bool operator==(const ConnectorRegistration &) const = default;
    };

    /// Fails when a binding slot is not a decimal uint32: every slot of the definition
    /// must reach the event, or the registered record would silently differ from it.
    std::optional<ConnectorRegistration> buildConnectorRegistration(const Connector & connector);

    /// keccak256(abi.encode(registration)), the value RegistryBase requires in
    /// Publication.registrationHash so the emitted arrays are the ones the owner authorized.
    evmc::bytes32 computeRegistrationHash(const ConnectorRegistration & registration);

    /// Content hash bound into a publication: keccak256 of the exact artifact bytes.
    evmc::bytes32 computeContentHash(const std::string & artifact_bytes);

    /// Metadata hash bound into a publication: keccak256 of canonical metadata JSON.
    evmc::bytes32 computeMetadataHash(EntityKind kind, const std::string & name);

    /// CREATE2 address RegistryBase deploys `publication` to:
    /// salt = keccak256(abi.encode(registry, kind, keccak256(name), owner, contentHash, nonce)).
    chain::Address predictEntityAddress(
        const chain::Address & registry_address,
        const Publication & publication,
        std::span<const std::uint8_t> creation_code);

    std::vector<std::uint8_t> encodePublishTransformationCall(
        std::span<const std::uint8_t> creation_code,
        const Publication & publication,
        std::span<const std::uint8_t> owner_signature);

    std::vector<std::uint8_t> encodePublishConditionCall(
        std::span<const std::uint8_t> creation_code,
        const Publication & publication,
        std::span<const std::uint8_t> owner_signature);

    std::vector<std::uint8_t> encodePublishConnectorCall(
        std::span<const std::uint8_t> creation_code,
        const Publication & publication,
        const ConnectorRegistration & registration,
        std::span<const std::uint8_t> owner_signature);

    /// The Publication a `publishTransformation`, `publishCondition` or `publishConnector`
    /// call carries, with the kind taken from the selector. Nullopt when the calldata is
    /// not one of those calls or its publication tuple is malformed. The creation code,
    /// connector registration and owner signature are not decoded. A deadline beyond 64
    /// bits, such as type(uint256).max, never expires and decodes as the largest uint64.
    std::optional<Publication> decodePublishCall(std::span<const std::uint8_t> calldata);

    /// The creation code of every publish call found anywhere in `calldata`, keyed by the
    /// address `registry` deploys it to with CREATE2: the transaction's own calldata when it
    /// called the registry directly, or the inner calls a relay (a multisig, a smart account,
    /// a multicall) embeds verbatim. The CREATE2 address binds the publication's salt fields
    /// and the creation code itself, so the code found for an entity's address is the code
    /// it was deployed from, whether or not that copy of the call is the one that executed.
    /// Empty when no publish call is present, e.g. a relay that builds the call on chain.
    std::map<chain::Address, std::vector<std::uint8_t>> findPublishedCreationCodes(
        std::span<const std::uint8_t> calldata,
        const chain::Address & registry);

    /// Calldata for `publicationNonce(address)`.
    std::vector<std::uint8_t> encodePublicationNonceCall(const chain::Address & owner);

    /// The EIP-712 digest an owner signs to let a third party relay the publication,
    /// matching RegistryBase.publicationDigest. The domain binds chain id and registry
    /// address, so a signature cannot be replayed onto another chain or registry.
    evmc::bytes32 publicationDigest(
        std::uint64_t chain_id,
        const chain::Address & registry_address,
        const Publication & publication);

    /// Recovers the signer of a 65-byte `r || s || v` secp256k1 signature over `digest`.
    /// High-s signatures are rejected, as the registry's ECDSA.recover rejects them.
    std::optional<chain::Address> recoverSigner(
        const evmc::bytes32 & digest,
        std::span<const std::uint8_t> signature);
}
