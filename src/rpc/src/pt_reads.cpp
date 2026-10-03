#include "pt_reads.hpp"

#include <format>
#include <vector>

#include "chain.hpp"
#include "condition.hpp"
#include "publication.hpp"
#include "registry_events.hpp"
#include "transformation.hpp"

namespace dcn::rpc
{
    asio::awaitable<std::expected<std::map<chain::Address, std::vector<std::uint8_t>>, RpcError>> readPublishedCreationCodes(
        const EthClient & client,
        const std::string_view tx_hash,
        const std::string_view block_hash,
        const chain::Address & registry)
    {
        using Result = std::map<chain::Address, std::vector<std::uint8_t>>;

        const auto transaction = co_await client.transactionByHash(tx_hash);
        if(!transaction) co_return std::unexpected(transaction.error());
        if(!*transaction)
        {
            co_return std::unexpected(RpcError::protocol(std::format("transaction {} is unknown", tx_hash)));
        }
        if(chain::normalizeHex((**transaction).block_hash) != chain::normalizeHex(std::string(block_hash)))
        {
            co_return std::unexpected(RpcError::protocol(std::format("transaction {} is not in block {}", tx_hash, block_hash)));
        }

        const auto calldata = evmc::from_hex((**transaction).input);
        if(!calldata) co_return Result{};
        co_return pt::findPublishedCreationCodes(*calldata, registry);
    }

    asio::awaitable<std::expected<std::optional<PublicationLocation>, RpcError>> findEntityPublication(
        const EthClient & client,
        const std::string_view registry_address,
        const pt::EntityKind kind,
        const std::string_view name,
        const chain::Address & entity_address,
        const std::int64_t latest_block)
    {
        using Result = std::optional<PublicationLocation>;
        const std::string entity_hex = chain::addressToHex(entity_address);

        const auto hasCode = [&client, &entity_hex](const std::int64_t block) -> asio::awaitable<std::expected<bool, RpcError>>
        {
            const auto code = co_await client.getCode(entity_hex, chain::toHexQuantity(block));
            if(!code) co_return std::unexpected(code.error());
            co_return *code != "0x";
        };

        // The code exists from its publication block on: the first block holding it.
        const auto at_latest = co_await hasCode(latest_block);
        if(!at_latest) co_return std::unexpected(at_latest.error());
        if(!*at_latest) co_return Result{};
        std::int64_t low = 0;
        std::int64_t high = latest_block;
        while(low < high)
        {
            const std::int64_t middle = low + (high - low) / 2;
            const auto present = co_await hasCode(middle);
            if(!present) co_return std::unexpected(present.error());
            if(*present) high = middle;
            else low = middle + 1;
        }

        const auto logs = co_await client.getLogs(LogFilter{
            .from_block = low,
            .to_block = low,
            .address = std::string(registry_address),
            .topic0 = {pt::eventTopic(pt::addedEventSignature(kind))}});
        if(!logs) co_return std::unexpected(logs.error());

        for(const json & log : *logs)
        {
            const auto data = log.find("data");
            const auto topics = log.find("topics");
            const auto tx_hash = log.find("transactionHash");
            const auto block_hash = log.find("blockHash");
            if(data == log.end() || topics == log.end() || tx_hash == log.end() || block_hash == log.end()
               || !data->is_string() || !topics->is_array() || !tx_hash->is_string() || !block_hash->is_string())
            {
                continue;
            }
            std::vector<std::string> topic_list;
            for(const json & topic : *topics)
            {
                if(topic.is_string()) topic_list.push_back(topic.get<std::string>());
            }

            bool matches = false;
            if(kind == pt::EntityKind::TRANSFORMATION)
            {
                const auto event = pt::decodeTransformationAddedEvent(data->get<std::string>(), topic_list);
                matches = event && event->name == name && event->transformation_address == entity_address;
            }
            else if(kind == pt::EntityKind::CONDITION)
            {
                const auto event = pt::decodeConditionAddedEvent(data->get<std::string>(), topic_list);
                matches = event && event->name == name && event->condition_address == entity_address;
            }
            if(matches)
            {
                co_return Result{PublicationLocation{
                    .tx_hash = tx_hash->get<std::string>(),
                    .block_hash = block_hash->get<std::string>()}};
            }
        }
        co_return Result{};
    }

    namespace
    {
        std::string _calldata(const std::string & signature, const std::vector<pt::abi::Component> & arguments)
        {
            std::vector<std::uint8_t> out = chain::constructSelector(signature);
            const std::vector<std::uint8_t> body = pt::abi::encodeTuple(arguments);
            out.insert(out.end(), body.begin(), body.end());
            return chain::bytesToHex(out);
        }

        std::string_view _kindSuffix(const pt::EntityKind kind)
        {
            switch(kind)
            {
            case pt::EntityKind::CONNECTOR: return "Connector";
            case pt::EntityKind::TRANSFORMATION: return "Transformation";
            case pt::EntityKind::CONDITION: return "Condition";
            }
            return "Transformation";
        }

        std::vector<pt::abi::Component> _entityKey(const pt::EntityKind kind, const std::string_view name)
        {
            return {
                pt::abi::uintComponent(static_cast<std::uint64_t>(kind)),
                pt::abi::stringComponent(std::string(name))};
        }
    }

    std::string ptProtocolId()
    {
        return chain::bytes32ToHex(chain::keccakString("hypermusic.pt"));
    }

    asio::awaitable<std::expected<PtIdentity, RpcError>> readPtIdentity(
        const EthClient & client,
        const std::string_view contract_address)
    {
        auto id_result = co_await client.call(contract_address, _calldata("protocolId()", {}));
        if(!id_result) co_return std::unexpected(id_result.error());

        const auto id_bytes = evmc::from_hex(*id_result);
        if(!id_bytes || id_bytes->size() != 32)
        {
            co_return std::unexpected(RpcError::protocol("protocolId() did not return one word"));
        }

        auto version_result = co_await client.call(contract_address, _calldata("protocolVersion()", {}));
        if(!version_result) co_return std::unexpected(version_result.error());

        const auto version = uint64FromWord(*version_result, "protocolVersion()");
        if(!version) co_return std::unexpected(version.error());

        co_return PtIdentity{
            .protocol_id = chain::normalizeHex(*id_result),
            .protocol_version = *version
        };
    }

    asio::awaitable<std::expected<PtIdentity, std::string>> verifyPtRegistry(
        const EthClient & client,
        const std::uint64_t expected_chain_id,
        const std::string_view registry_address)
    {
        const auto chain_id = co_await client.chainId();
        if(!chain_id)
        {
            co_return std::unexpected(std::format("eth_chainId failed: {}", chain_id.error().message));
        }

        if(*chain_id != expected_chain_id)
        {
            co_return std::unexpected(std::format(
                "configured chain id {} does not match endpoint chain id {}", expected_chain_id, *chain_id));
        }

        const auto code = co_await client.getCode(registry_address);
        if(!code)
        {
            co_return std::unexpected(std::format("eth_getCode failed: {}", code.error().message));
        }

        if(*code == "0x" || *code == "0X")
        {
            co_return std::unexpected("configured registry address has no deployed code");
        }

        const auto identity = co_await readPtIdentity(client, registry_address);
        if(!identity)
        {
            co_return std::unexpected(std::format(
                "registry does not expose PT protocol identity: {}", identity.error().message));
        }

        if(identity->protocol_id != ptProtocolId())
        {
            co_return std::unexpected("registry protocolId() is not the PT protocol identifier");
        }

        if(identity->protocol_version != PT_PROTOCOL_VERSION)
        {
            co_return std::unexpected(std::format(
                "registry protocol version {} is not supported by this server (expected {})",
                identity->protocol_version,
                PT_PROTOCOL_VERSION));
        }

        co_return *identity;
    }

    asio::awaitable<std::expected<void, std::string>> verifyPtRunner(
        const EthClient & client,
        const std::string_view runner_address)
    {
        const auto identity = co_await readPtIdentity(client, runner_address);
        if(!identity)
        {
            co_return std::unexpected(std::format(
                "runner does not expose PT protocol identity: {}", identity.error().message));
        }
        if(identity->protocol_id != ptProtocolId() || identity->protocol_version != PT_PROTOCOL_VERSION)
        {
            co_return std::unexpected("runner protocol identity is not the supported PT protocol");
        }

        co_return std::expected<void, std::string>{};
    }

    asio::awaitable<std::expected<std::string, RpcError>> readOwner(
        const EthClient & client,
        const std::string_view contract_address)
    {
        auto result = co_await client.call(contract_address, _calldata("getOwner()", {}));
        if(!result) co_return std::unexpected(result.error());

        co_return addressFromWord(*result, "getOwner()");
    }

    asio::awaitable<std::expected<std::uint64_t, RpcError>> readPublicationNonce(
        const EthClient & client,
        const std::string_view registry_address,
        const std::string_view owner_address)
    {
        const auto owner = evmc::from_hex<chain::Address>(std::string(owner_address));
        if(!owner)
        {
            co_return std::unexpected(RpcError::protocol("publication owner is not an address"));
        }

        // Calldata is built before the co_await: GCC cannot compile an initializer_list
        // that lives across one.
        const std::string calldata = _calldata("publicationNonce(address)", {pt::abi::addressComponent(*owner)});
        auto result = co_await client.call(registry_address, calldata);
        if(!result) co_return std::unexpected(result.error());

        co_return uint64FromWord(*result, "publicationNonce(address)");
    }

    asio::awaitable<std::expected<bool, RpcError>> registryContainsEntity(
        const EthClient & client,
        const std::string_view registry_address,
        const pt::EntityKind kind,
        const std::string_view name,
        const std::string_view tag)
    {
        const std::string signature = std::format("contains{}(string)", _kindSuffix(kind));

        const std::string calldata = _calldata(signature, {pt::abi::stringComponent(std::string(name))});
        auto result = co_await client.call(registry_address, calldata, tag);
        if(!result) co_return std::unexpected(result.error());

        const auto value = uint64FromWord(*result, signature);
        if(!value || *value > 1)
        {
            co_return std::unexpected(RpcError::protocol(std::format("{} did not return a bool", signature)));
        }

        co_return *value == 1;
    }

    asio::awaitable<std::expected<std::string, RpcError>> readEntityContentHash(
        const EthClient & client,
        const std::string_view registry_address,
        const pt::EntityKind kind,
        const std::string_view name)
    {
        auto result = co_await client.call(
            registry_address, _calldata("getEntityContentHash(uint8,string)", _entityKey(kind, name)));
        if(!result) co_return std::unexpected(result.error());

        const auto bytes = evmc::from_hex(*result);
        if(!bytes || bytes->size() != 32)
        {
            co_return std::unexpected(RpcError::protocol("getEntityContentHash did not return one word"));
        }

        co_return chain::normalizeHex(*result);
    }

    asio::awaitable<std::expected<std::string, RpcError>> readEntityOwner(
        const EthClient & client,
        const std::string_view registry_address,
        const pt::EntityKind kind,
        const std::string_view name)
    {
        auto result = co_await client.call(
            registry_address, _calldata("getEntityOwner(uint8,string)", _entityKey(kind, name)));
        if(!result) co_return std::unexpected(result.error());

        co_return addressFromWord(*result, "getEntityOwner");
    }

    asio::awaitable<std::expected<std::string, RpcError>> readEntityAddress(
        const EthClient & client,
        const std::string_view registry_address,
        const pt::EntityKind kind,
        const std::string_view name)
    {
        const std::string signature = std::format("get{}(string)", _kindSuffix(kind));

        const std::string calldata = _calldata(signature, {pt::abi::stringComponent(std::string(name))});
        auto result = co_await client.call(registry_address, calldata);
        if(!result) co_return std::unexpected(result.error());

        co_return addressFromWord(*result, signature);
    }
}
