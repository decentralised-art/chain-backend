#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio.hpp>

#include "artifact.hpp"
#include "eth_client.hpp"
#include "events_ingest.hpp"
#include "publication.hpp"

#include "manifest.hpp"

namespace dcn::publish
{
    struct PublisherConfig
    {
        // Validity window baked into the publication: the registry refuses the
        // transaction once block.timestamp passes it.
        std::uint64_t publication_ttl_s = 3'600;

        std::uint64_t gas_limit_headroom_percent = 20;
    };

    struct PublishError
    {
        enum class Kind : std::uint8_t
        {
            INVALID,    // the request or artifact cannot produce a valid publication
            CONFLICT,   // the target registry holds something else under this name or a dependency's
            REVERTED,   // the transaction reverts, before or after it was mined
            FORBIDDEN,  // the transaction is signed by an account other than the entity owner
            PENDING,    // the transaction is not mined yet; outcome unknown
            UPSTREAM    // provider or durable storage failure
        };

        Kind kind = Kind::UPSTREAM;
        std::string message;
        std::string tx_hash;

        // Connector dependencies absent from, or different on, the target registry.
        std::vector<std::string> missing;
        std::vector<std::string> mismatched;
    };

    struct PublishResult
    {
        std::string tx_hash;
        std::string entity_address;
        std::int64_t block_number = 0;
        std::string owner;
        std::string content_hash;
        // True when the registry already held this exact publication.
        bool already_published = false;
    };

    /// Everything the registry factory needs for one entity, built from a verified
    /// artifact bundle by the loader. Nonce and deadline are filled in by prepare().
    struct PublicationRequest
    {
        pt::Publication publication;
        std::vector<std::uint8_t> creation_code;
        std::optional<pt::ConnectorRegistration> registration;  // connectors only
        std::string artifact_bytes;
        std::vector<pt::ArtifactDependency> dependencies;       // connectors only
    };

    /// What an offline signer needs beyond PreparedTransaction's fields to produce the
    /// raw type-2 transaction Publisher::send accepts. A browser wallet fills these in
    /// itself.
    struct SigningFields
    {
        std::uint64_t nonce = 0;   // the owner's pending account nonce
        std::uint64_t max_fee_per_gas = 0;
        std::uint64_t max_priority_fee_per_gas = 0;
    };

    /// The transaction the owner's wallet sends. The owner pays for it, and the registry
    /// accepts the empty owner signature because msg.sender is the owner.
    struct PreparedTransaction
    {
        std::string from;
        std::string to;
        std::string data;
        std::uint64_t chain_id = 0;
        std::uint64_t gas = 0;

        std::string entity_address;   // deterministic CREATE2 address
        std::string content_hash;
        std::uint64_t publication_nonce = 0;
        std::uint64_t deadline = 0;

        // Present when the publication was prepared for relay through send().
        std::optional<SigningFields> signing;
    };

    /// Identifies the publication a confirmation request is about.
    struct PublicationClaim
    {
        pt::EntityKind kind = pt::EntityKind::TRANSFORMATION;
        std::string name;
        chain::Address owner{};
        std::string content_hash;
        std::string metadata_hash;
    };

    /// Prepares publications for the owner's wallet and confirms them from the mined
    /// receipt. It holds no key: users pay for their own publications.
    class Publisher
    {
        public:
            Publisher(
                std::shared_ptr<rpc::EthClient> client,
                DeploymentManifest manifest,
                const pt::ArtifactStore & artifacts,
                PublisherConfig config = {});

            const DeploymentManifest & manifest() const;
            const chain::Address & registryAddress() const;

            /// The registry's current answer for this name: nullopt when free, the existing
            /// publication when it is this exact one, CONFLICT when it is something else.
            asio::awaitable<std::expected<std::optional<PublishResult>, PublishError>> findPublished(
                const pt::Publication & publication) const;

            /// Checks dependencies, uploads the artifact durably, binds the owner's current
            /// publication nonce and a deadline, and returns the transaction to send. With
            /// `relay`, also returns the account nonce and fees an offline signer needs.
            asio::awaitable<std::expected<PreparedTransaction, PublishError>> prepare(
                PublicationRequest request,
                bool relay = false) const;

            /// Broadcasts a publication transaction the owner signed offline, once it is
            /// proven to be the owner publishing exactly `claim` on the configured registry
            /// and a dry run shows it would not revert. Returns the transaction hash. A
            /// transaction the provider already holds is not an error, so a retried request
            /// is harmless. The owner pays for it; the server only forwards it.
            asio::awaitable<std::expected<std::string, PublishError>> send(
                PublicationClaim claim,
                std::string raw_tx) const;

            /// Looks the owner's transaction up once and confirms it registered exactly this
            /// publication; PENDING while it is unmined. Repeating the call is the status
            /// operation.
            asio::awaitable<std::expected<PublishResult, PublishError>> confirm(
                PublicationClaim claim,
                std::string tx_hash) const;

        private:
            std::expected<PublishResult, PublishError> _validateReceipt(
                const rpc::TransactionReceipt & receipt,
                const PublicationClaim & claim) const;

            std::shared_ptr<rpc::EthClient> _client;
            DeploymentManifest _manifest;
            const pt::ArtifactStore & _artifacts;
            PublisherConfig _config;
            events::PTEventDecoder _decoder;
            chain::Address _registry_address{};
    };

    /// Decodes an `Error(string)` revert body into something an operator can act on.
    std::string describeRevert(const std::string & data_hex);
}
