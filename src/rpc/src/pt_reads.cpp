#include "pt_reads.hpp"

#include <format>
#include <vector>

#include "chain.hpp"
#include "publication.hpp"

namespace dcn::rpc
{
    namespace
    {
        RpcError _protocolError(std::string message)
        {
            return RpcError{.kind = RpcError::Kind::PROTOCOL, .code = 0, .message = std::move(message)};
        }

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
            co_return std::unexpected(_protocolError("protocolId() did not return one word"));
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
        const std::string_view runner_address,
        const std::string_view registry_address)
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

        const auto linked = co_await readRunnerRegistry(client, runner_address);
        if(!linked)
        {
            co_return std::unexpected(std::format("runner registry() is unreadable: {}", linked.error().message));
        }
        if(*linked != registry_address)
        {
            co_return std::unexpected("runner is linked to a different registry");
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

    asio::awaitable<std::expected<std::string, RpcError>> readRunnerRegistry(
        const EthClient & client,
        const std::string_view runner_address)
    {
        auto result = co_await client.call(runner_address, _calldata("registry()", {}));
        if(!result) co_return std::unexpected(result.error());

        co_return addressFromWord(*result, "registry()");
    }

    asio::awaitable<std::expected<std::uint64_t, RpcError>> readPublicationNonce(
        const EthClient & client,
        const std::string_view registry_address,
        const std::string_view owner_address)
    {
        const auto owner = evmc::from_hex<chain::Address>(std::string(owner_address));
        if(!owner)
        {
            co_return std::unexpected(_protocolError("publication owner is not an address"));
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
            co_return std::unexpected(_protocolError(std::format("{} did not return a bool", signature)));
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
            co_return std::unexpected(_protocolError("getEntityContentHash did not return one word"));
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
