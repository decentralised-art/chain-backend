#include <algorithm>
#include <chrono>
#include <format>

#include <spdlog/spdlog.h>

#include "chain.hpp"
#include "pt_reads.hpp"
#include "registry_events.hpp"

#include "publisher.hpp"

namespace dcn::publish
{
    namespace
    {
        PublishError _upstream(std::string message, std::string tx_hash = {})
        {
            return PublishError{
                .kind = PublishError::Kind::UPSTREAM,
                .message = std::move(message),
                .tx_hash = std::move(tx_hash)};
        }

        PublishError _invalid(std::string message, std::string tx_hash = {})
        {
            return PublishError{
                .kind = PublishError::Kind::INVALID,
                .message = std::move(message),
                .tx_hash = std::move(tx_hash)};
        }

        std::uint64_t _unixNowSeconds()
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        events::EventType _addedEventType(const pt::EntityKind kind)
        {
            switch(kind)
            {
            case pt::EntityKind::CONNECTOR: return events::EventType::CONNECTOR_ADDED;
            case pt::EntityKind::TRANSFORMATION: return events::EventType::TRANSFORMATION_ADDED;
            case pt::EntityKind::CONDITION: return events::EventType::CONDITION_ADDED;
            }
            return events::EventType::TRANSFORMATION_ADDED;
        }

        const std::string & _zeroHash()
        {
            static const std::string zero = chain::bytes32ToHex(evmc::bytes32{});
            return zero;
        }

        bool _isTransactionHash(const std::string & value)
        {
            const auto bytes = evmc::from_hex(value);
            return value.starts_with("0x") && bytes && bytes->size() == 32;
        }
    }

    std::string describeRevert(const std::string & data_hex)
    {
        const auto bytes = evmc::from_hex(data_hex);
        if(!bytes || bytes->size() < 4) return "transaction reverted";

        if(const auto reason = chain::decodeRevertReason(*bytes))
        {
            return std::format("transaction reverted: {}", *reason);
        }
        return std::format("transaction reverted: {}", data_hex);
    }

    Publisher::Publisher(
        std::shared_ptr<rpc::EthClient> client,
        DeploymentManifest manifest,
        const pt::ArtifactStore & artifacts,
        PublisherConfig config)
        : _client(std::move(client))
        , _manifest(std::move(manifest))
        , _artifacts(artifacts)
        , _config(config)
    {
        if(const auto parsed = evmc::from_hex<chain::Address>(_manifest.registry))
        {
            _registry_address = *parsed;
        }
    }

    const DeploymentManifest & Publisher::manifest() const
    {
        return _manifest;
    }

    const chain::Address & Publisher::registryAddress() const
    {
        return _registry_address;
    }

    asio::awaitable<std::expected<std::optional<PublishResult>, PublishError>> Publisher::findPublished(
        const pt::Publication & publication) const
    {
        if(_registry_address == chain::Address{})
        {
            co_return std::unexpected(_upstream("no registry address is configured"));
        }

        const auto contains = co_await rpc::registryContainsEntity(
            *_client, _manifest.registry, publication.kind, publication.name);
        if(!contains)
        {
            co_return std::unexpected(_upstream(
                std::format("registry membership check failed: {}", contains.error().message)));
        }
        if(!*contains)
        {
            co_return std::optional<PublishResult>{};
        }

        const std::string expected_content = chain::bytes32ToHex(publication.content_hash);
        const auto recorded = co_await rpc::readEntityContentHash(
            *_client, _manifest.registry, publication.kind, publication.name);
        if(!recorded)
        {
            co_return std::unexpected(_upstream(
                std::format("getEntityContentHash failed: {}", recorded.error().message)));
        }
        if(*recorded != expected_content)
        {
            co_return std::unexpected(PublishError{
                .kind = PublishError::Kind::CONFLICT,
                .message = std::format(
                    "'{}' already exists on the target registry with a different identity", publication.name)});
        }

        const auto address = co_await rpc::readEntityAddress(
            *_client, _manifest.registry, publication.kind, publication.name);
        if(!address)
        {
            co_return std::unexpected(_upstream(
                std::format("registry entity lookup failed: {}", address.error().message)));
        }

        const auto owner = co_await rpc::readEntityOwner(
            *_client, _manifest.registry, publication.kind, publication.name);
        if(!owner)
        {
            co_return std::unexpected(_upstream(
                std::format("registry owner lookup failed: {}", owner.error().message)));
        }

        co_return std::optional<PublishResult>{PublishResult{
            .entity_address = *address,
            .owner = *owner,
            .content_hash = expected_content,
            .already_published = true}};
    }

    asio::awaitable<std::expected<PreparedTransaction, PublishError>> Publisher::prepare(
        PublicationRequest request) const
    {
        pt::Publication & publication = request.publication;

        if(_registry_address == chain::Address{})
        {
            co_return std::unexpected(_upstream("no registry address is configured"));
        }
        if(request.creation_code.empty())
        {
            co_return std::unexpected(_invalid("creation code is empty"));
        }
        if(!pt::isValidEntityName(publication.name))
        {
            co_return std::unexpected(_invalid("entity name is not a valid identifier"));
        }

        if(publication.kind == pt::EntityKind::CONNECTOR)
        {
            if(!request.registration)
            {
                co_return std::unexpected(_invalid("connector publication requires a registration"));
            }

            // A connector binds its dependencies by name on the target registry, so each one
            // must already be the exact artifact the connector was compiled against.
            PublishError conflict{
                .kind = PublishError::Kind::CONFLICT,
                .message = "Dependencies are missing from, or differ on, the target registry"};
            for(const pt::ArtifactDependency & dependency : request.dependencies)
            {
                const auto recorded = co_await rpc::readEntityContentHash(
                    *_client, _manifest.registry, dependency.kind, dependency.name);
                if(!recorded)
                {
                    co_return std::unexpected(_upstream(std::format(
                        "dependency lookup for '{}' failed: {}", dependency.name, recorded.error().message)));
                }
                if(*recorded == _zeroHash())
                {
                    conflict.missing.push_back(dependency.name);
                }
                else if(*recorded != dependency.content_hash)
                {
                    conflict.mismatched.push_back(dependency.name);
                }
            }
            if(!conflict.missing.empty() || !conflict.mismatched.empty())
            {
                co_return std::unexpected(std::move(conflict));
            }

            publication.registration_hash = pt::computeRegistrationHash(*request.registration);
        }
        else
        {
            publication.registration_hash = evmc::bytes32{};
        }

        const std::string owner_hex = chain::addressToHex(publication.owner);
        const auto nonce = co_await rpc::readPublicationNonce(*_client, _manifest.registry, owner_hex);
        if(!nonce)
        {
            co_return std::unexpected(_upstream(
                std::format("cannot read the publication nonce: {}", nonce.error().message)));
        }
        publication.nonce = *nonce;
        publication.deadline = _unixNowSeconds() + _config.publication_ttl_s;

        // Durable availability is proven before the owner can broadcast: an event whose
        // artifact cannot be fetched is indexable but not reconstructable.
        const std::string content_hash = chain::bytes32ToHex(publication.content_hash);
        const auto uploaded = _artifacts.publish(request.artifact_bytes);
        if(!uploaded)
        {
            co_return std::unexpected(_upstream(std::format("durable artifact upload failed: {}", uploaded.error())));
        }
        if(*uploaded != content_hash)
        {
            co_return std::unexpected(_invalid("stored artifact does not match its hash"));
        }

        std::vector<std::uint8_t> calldata;
        switch(publication.kind)
        {
        case pt::EntityKind::TRANSFORMATION:
            calldata = pt::encodePublishTransformationCall(request.creation_code, publication, {});
            break;
        case pt::EntityKind::CONDITION:
            calldata = pt::encodePublishConditionCall(request.creation_code, publication, {});
            break;
        case pt::EntityKind::CONNECTOR:
            calldata = pt::encodePublishConnectorCall(
                request.creation_code, publication, *request.registration, {});
            break;
        }
        const std::string data_hex = chain::bytesToHex(calldata);

        // A deterministic revert (name taken, stale nonce, identity mismatch) is reported
        // now instead of after the owner paid for a failing transaction.
        const auto estimate = co_await _client->estimateGas(owner_hex, _manifest.registry, data_hex);
        if(!estimate)
        {
            if(estimate.error().kind == rpc::RpcError::Kind::EXECUTION)
            {
                co_return std::unexpected(PublishError{
                    .kind = PublishError::Kind::REVERTED,
                    .message = estimate.error().data.empty()
                        ? std::format("publication would revert: {}", estimate.error().message)
                        : describeRevert(estimate.error().data)});
            }
            co_return std::unexpected(_upstream(
                std::format("eth_estimateGas failed: {}", estimate.error().message)));
        }

        co_return PreparedTransaction{
            .from = owner_hex,
            .to = _manifest.registry,
            .data = data_hex,
            .chain_id = _manifest.chain_id,
            .gas = *estimate + (*estimate * _config.gas_limit_headroom_percent) / 100,
            .entity_address = chain::addressToHex(
                pt::predictEntityAddress(_registry_address, publication, request.creation_code)),
            .content_hash = content_hash,
            .publication_nonce = publication.nonce,
            .deadline = publication.deadline
        };
    }

    std::expected<PublishResult, PublishError> Publisher::_validateReceipt(
        const rpc::TransactionReceipt & receipt,
        const PublicationClaim & claim) const
    {
        const std::string expected_topic = pt::eventTopic(pt::addedEventSignature(claim.kind));
        const events::EventType expected_type = _addedEventType(claim.kind);

        std::optional<events::DecodedEvent> matched;
        for(const auto & log_json : receipt.logs)
        {
            const auto raw = parse::parseRawLog(log_json, 0, _manifest.chain_id);
            if(!raw) continue;

            // Only the configured registry can register an entity; a log from any other
            // address in the same transaction is not evidence of a registration.
            if(raw->address != _manifest.registry) continue;
            if(!raw->topics[0] || *raw->topics[0] != expected_topic) continue;

            const auto decoded = _decoder.decode(*raw);
            if(!decoded || decoded->event_type != expected_type) continue;

            if(matched)
            {
                return std::unexpected(_invalid("receipt carries more than one registry event", receipt.tx_hash));
            }
            matched = *decoded;
        }

        if(!matched)
        {
            return std::unexpected(_invalid("transaction did not register a publication", receipt.tx_hash));
        }

        if(matched->name != claim.name || matched->owner != chain::addressToHex(claim.owner)
           || matched->content_hash.value_or("") != claim.content_hash
           || matched->metadata_hash.value_or("") != claim.metadata_hash)
        {
            return std::unexpected(_invalid("transaction registered a different publication", receipt.tx_hash));
        }

        return PublishResult{
            .tx_hash = receipt.tx_hash,
            .entity_address = matched->entity_address,
            .block_number = receipt.block_number,
            .owner = matched->owner,
            .content_hash = claim.content_hash
        };
    }

    asio::awaitable<std::expected<PublishResult, PublishError>> Publisher::confirm(
        PublicationClaim claim,
        std::string tx_hash) const
    {
        tx_hash = chain::normalizeHex(std::move(tx_hash));
        if(!_isTransactionHash(tx_hash))
        {
            co_return std::unexpected(_invalid("'tx_hash' is not a transaction hash"));
        }

        const auto receipt = co_await _client->transactionReceipt(tx_hash);
        if(!receipt)
        {
            co_return std::unexpected(_upstream(
                std::format("receipt lookup failed: {}", receipt.error().message), tx_hash));
        }
        if(!*receipt)
        {
            co_return std::unexpected(PublishError{
                .kind = PublishError::Kind::PENDING,
                .message = "the transaction is not mined yet",
                .tx_hash = tx_hash});
        }

        if(!(*receipt)->success)
        {
            co_return std::unexpected(PublishError{
                .kind = PublishError::Kind::REVERTED,
                .message = "transaction reverted",
                .tx_hash = tx_hash});
        }

        co_return _validateReceipt(**receipt, claim);
    }
}
