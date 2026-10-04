#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "events_test_harness.hpp"

#include "eth_client.hpp"
#include "json_rpc.hpp"
#include "publish.hpp"

namespace dcn::tests::fake_eth
{
    using namespace dcn::tests::events_harness;

    inline const std::string REGISTRY_ADDRESS = hexAddress(0xAB);
    inline const std::string RUNNER_ADDRESS = hexAddress(0xAC);
    inline const std::string REGISTRY_IMPL_ADDRESS = hexAddress(0xAD);
    inline const std::string RUNNER_IMPL_ADDRESS = hexAddress(0xAE);

    inline std::string selectorOf(const std::string & signature)
    {
        const auto selector = chain::constructSelector(signature);
        return toHexPrefixed(std::span<const std::uint8_t>(selector.data(), selector.size()));
    }

    inline std::string wordOfAddress(const std::string & address_hex)
    {
        const auto address = evmc::from_hex<chain::Address>(address_hex).value();
        return toHexPrefixed(std::span<const std::uint8_t>(encodeAddressWord(address)));
    }

    inline std::string wordOfUint(const std::uint64_t value)
    {
        return toHexPrefixed(std::span<const std::uint8_t>(encodeUint256Word(value)));
    }

    inline std::string wordOfFill(const std::uint8_t fill)
    {
        return toHexPrefixed(std::span<const std::uint8_t>(encodeBytes32Word(fill)));
    }

    inline evmc::bytes32 bytes32OfFill(const std::uint8_t fill)
    {
        evmc::bytes32 out{};
        for(std::uint8_t & byte : out.bytes) byte = fill;
        return out;
    }

    // The entity name of a `(uint8 kind, string name)` registry call.
    inline std::string nameFromEntityCall(const std::string & data_hex)
    {
        const auto bytes = evmc::from_hex(data_hex).value();
        const std::uint8_t * arguments = bytes.data() + 4;
        const std::size_t size = bytes.size() - 4;
        const auto name_offset = chain::readWordAsSizeT(arguments, size, 32).value();
        return chain::decodeAbiString(arguments, size, name_offset).value_or("");
    }

    inline std::string rpcResult(const json & request, json result)
    {
        return json{{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}}.dump();
    }

    inline std::string rpcError(const json & request, const int code, const std::string & message, const std::string & data = {})
    {
        json error{{"code", code}, {"message", message}};
        if(!data.empty()) error["data"] = data;
        return json{{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"error", std::move(error)}}.dump();
    }

    // A JSON-RPC node good enough to drive deployment verification, publication
    // preparation and confirmation, contract deployment and registry reads end to end, with
    // per-test control over what the chain reports.
    struct FakeNode
    {
        std::uint64_t chain_id = CHAIN_ID;
        std::int64_t head_block = 100;
        std::uint64_t base_fee = 1'000'000'000;
        std::uint64_t priority_fee = 1'500'000'000;
        std::uint64_t tx_count = 7;
        std::uint64_t gas_estimate = 150'000;
        std::optional<rpc::RpcError> estimate_error;
        // Answered for eth_sendRawTransaction instead of accepting the transaction.
        std::optional<rpc::RpcError> send_error;

        bool contains_entity = false;
        std::string entity_content_hash = wordOfFill(0xAA);
        std::map<std::string, std::string> content_hash_by_name;
        std::string entity_address = wordOfAddress(hexAddress(0x55));
        std::string entity_owner = wordOfAddress(hexAddress(0x42));

        std::string code = "0x60006000";
        std::map<std::string, std::string> code_by_address;
        // An address listed here has no code before this block number.
        std::map<std::string, std::int64_t> code_created_at;

        // Answers eth_getTransactionByHash; register a transaction with addTransaction.
        std::map<std::string, json> transactions_by_hash;
        // Answers eth_getLogs over a single block number.
        std::map<std::int64_t, json> logs_by_block_number;

        void addTransaction(const std::string & tx_hash, const std::string & block_hash, const std::vector<std::uint8_t> & input = {})
        {
            transactions_by_hash[chain::normalizeHex(tx_hash)] = json{
                {"hash", tx_hash},
                {"blockHash", block_hash},
                {"input", chain::bytesToHex(input)}};
        }
        std::string implementation_slot = wordOfAddress(REGISTRY_IMPL_ADDRESS);
        std::map<std::string, std::string> implementation_by_proxy;
        std::string owner_word = wordOfAddress(hexAddress(0x77));
        std::uint64_t publication_nonce = 3;

        // Answered for every receipt lookup unless a deployment receipt was recorded for
        // the hash. A null receipt means "not mined yet".
        json receipt = json{};
        std::map<std::string, json> receipts_by_hash;

        // When set, every broadcast transaction is a contract creation mined at the next
        // address, one block after the previous one.
        bool mine_deployments = false;
        std::uint8_t next_contract_byte = 0xA0;

        // Called with every accepted raw transaction, to change chain state as it would.
        std::function<void(const std::string & raw_tx)> on_send;

        // Consulted first for eth_call; an empty answer falls through to the registry reads.
        std::function<std::string(const json & request)> call_override;
        // Unless set, a membership check answers from contains_entity.
        std::optional<rpc::RpcError> contains_error;

        // False answers a named block tag ("safe", "finalized", ...) with null.
        bool serve_block_tags = true;

        std::vector<std::string> sent_raw_transactions;
        std::vector<json> calls;
        std::vector<json> code_requests;
        std::vector<json> transaction_requests;
        std::vector<std::string> requested_blocks;

        // Consulted before every request; an error fails it at the transport.
        std::function<std::optional<rpc::RpcError>(const json & request)> transport_fault;

        std::string operator()(const json & request)
        {
            const std::string method = request.value("method", "");
            const json params = request.value("params", json::array());

            if(method == "eth_chainId") return rpcResult(request, chain::toHexQuantity(static_cast<std::int64_t>(chain_id)));
            if(method == "eth_blockNumber") return rpcResult(request, chain::toHexQuantity(head_block));
            if(method == "eth_maxPriorityFeePerGas") return rpcResult(request, chain::toHexQuantity(static_cast<std::int64_t>(priority_fee)));
            if(method == "eth_getTransactionCount") return rpcResult(request, chain::toHexQuantity(static_cast<std::int64_t>(tx_count)));
            if(method == "eth_getCode")
            {
                code_requests.push_back(params);
                const std::string address = chain::normalizeHex(params.at(0).get<std::string>());
                if(const auto created = code_created_at.find(address);
                   created != code_created_at.end() && params.at(1).is_string())
                {
                    const auto block = parse::parseHexQuantity(params.at(1).get<std::string>());
                    if(block && *block < created->second) return rpcResult(request, "0x");
                }
                const auto found = code_by_address.find(address);
                return rpcResult(request, found != code_by_address.end() ? found->second : code);
            }

            if(method == "eth_getTransactionByHash")
            {
                transaction_requests.push_back(params);
                const auto found = transactions_by_hash.find(chain::normalizeHex(params.at(0).get<std::string>()));
                return rpcResult(request, found != transactions_by_hash.end() ? found->second : json{});
            }

            if(method == "eth_getLogs" && params.at(0).contains("fromBlock"))
            {
                const auto from = parse::parseHexQuantity(params.at(0).at("fromBlock").get<std::string>());
                const auto found = from ? logs_by_block_number.find(*from) : logs_by_block_number.end();
                return rpcResult(request, found != logs_by_block_number.end() ? found->second : json::array());
            }

            if(method == "eth_getStorageAt")
            {
                const auto proxy = implementation_by_proxy.find(chain::normalizeHex(params.at(0).get<std::string>()));
                return rpcResult(request, proxy != implementation_by_proxy.end() ? proxy->second : implementation_slot);
            }

            if(method == "eth_estimateGas")
            {
                if(estimate_error)
                {
                    return rpcError(request, 3, estimate_error->message, estimate_error->data);
                }
                return rpcResult(request, chain::toHexQuantity(static_cast<std::int64_t>(gas_estimate)));
            }

            if(method == "eth_getBlockByNumber")
            {
                const std::string block = params.at(0).get<std::string>();
                requested_blocks.push_back(block);
                if(!serve_block_tags && !block.starts_with("0x"))
                {
                    return rpcResult(request, json(nullptr));
                }
                return rpcResult(request, json{
                    {"number", chain::toHexQuantity(head_block)},
                    {"hash", hexBytes(0x31, 32)},
                    {"parentHash", hexBytes(0x30, 32)},
                    {"timestamp", chain::toHexQuantity(1'700'000'000 + head_block)},
                    {"baseFeePerGas", chain::toHexQuantity(static_cast<std::int64_t>(base_fee))}});
            }

            if(method == "eth_sendRawTransaction")
            {
                if(send_error) return rpcError(request, send_error->code, send_error->message);
                sent_raw_transactions.push_back(params.at(0).get<std::string>());
                const std::string tx_hash = publish::transactionHash(sent_raw_transactions.back());
                if(mine_deployments)
                {
                    receipts_by_hash[chain::normalizeHex(tx_hash)] = json{
                        {"status", "0x1"},
                        {"transactionHash", tx_hash},
                        {"blockNumber", chain::toHexQuantity(static_cast<std::int64_t>(50 + sent_raw_transactions.size()))},
                        {"blockHash", hexBytes(0x41, 32)},
                        {"gasUsed", "0x5208"},
                        {"contractAddress", hexAddress(next_contract_byte++)},
                        {"logs", json::array()}};
                }
                if(on_send) on_send(sent_raw_transactions.back());
                return rpcResult(request, tx_hash);
            }

            if(method == "eth_getTransactionReceipt")
            {
                const auto recorded = receipts_by_hash.find(chain::normalizeHex(params.at(0).get<std::string>()));
                return rpcResult(request, recorded != receipts_by_hash.end() ? recorded->second : receipt);
            }

            if(method == "eth_call")
            {
                calls.push_back(params);
                if(call_override)
                {
                    if(std::string answer = call_override(request); !answer.empty()) return answer;
                }

                const std::string data = params.at(0).value("data", "");
                if(data.starts_with(selectorOf("containsTransformation(string)"))
                   || data.starts_with(selectorOf("containsCondition(string)"))
                   || data.starts_with(selectorOf("containsConnector(string)")))
                {
                    if(contains_error) return rpcError(request, contains_error->code, contains_error->message);
                    return rpcResult(request, wordOfUint(contains_entity ? 1 : 0));
                }
                if(data.starts_with(selectorOf("getEntityContentHash(uint8,string)")))
                {
                    const auto named = content_hash_by_name.find(nameFromEntityCall(data));
                    return rpcResult(request, named != content_hash_by_name.end() ? named->second : entity_content_hash);
                }
                if(data.starts_with(selectorOf("getEntityOwner(uint8,string)"))) return rpcResult(request, entity_owner);
                if(data.starts_with(selectorOf("getTransformation(string)"))
                   || data.starts_with(selectorOf("getCondition(string)"))
                   || data.starts_with(selectorOf("getConnector(string)")))
                {
                    return rpcResult(request, entity_address);
                }
                if(data.starts_with(selectorOf("publicationNonce(address)"))) return rpcResult(request, wordOfUint(publication_nonce));
                if(data.starts_with(selectorOf("protocolId()"))) return rpcResult(request, rpc::ptProtocolId());
                if(data.starts_with(selectorOf("protocolVersion()"))) return rpcResult(request, wordOfUint(rpc::PT_PROTOCOL_VERSION));
                if(data.starts_with(selectorOf("getOwner()"))) return rpcResult(request, owner_word);
                return rpcError(request, 3, "unexpected eth_call");
            }

            return rpcError(request, -32601, "method not found");
        }
    };

    class FakeTransport final : public rpc::ITransport
    {
        public:
            explicit FakeTransport(std::shared_ptr<FakeNode> node) : _node(std::move(node)) {}

            asio::awaitable<std::expected<std::string, rpc::RpcError>> post(std::string body) override
            {
                const json request = json::parse(body, nullptr, false);
                if(_node->transport_fault)
                {
                    if(auto fault = _node->transport_fault(request)) co_return std::unexpected(std::move(*fault));
                }
                co_return (*_node)(request);
            }

            std::string_view describe() const override { return "fake://provider"; }

        private:
            std::shared_ptr<FakeNode> _node;
    };

    inline std::shared_ptr<rpc::EthClient> makeClient(std::shared_ptr<FakeNode> node)
    {
        return std::make_shared<rpc::EthClient>(
            std::make_shared<rpc::JsonRpcClient>(
                std::make_shared<FakeTransport>(std::move(node)),
                rpc::JsonRpcConfig{.max_attempts = 2, .initial_backoff_ms = 0, .max_backoff_ms = 0}));
    }

    inline publish::DeploymentManifest makeManifest()
    {
        return publish::DeploymentManifest{
            .chain_id = CHAIN_ID,
            .registry = REGISTRY_ADDRESS,
            .runner = RUNNER_ADDRESS,
            .registry_impl = REGISTRY_IMPL_ADDRESS,
            .runner_impl = RUNNER_IMPL_ADDRESS,
            .registry_impl_code_hash = chain::bytes32ToHex(chain::keccakString("registry-impl")),
            .runner_impl_code_hash = chain::bytes32ToHex(chain::keccakString("runner-impl")),
            .deployment_block = 42,
            .protocol_id = rpc::ptProtocolId(),
            .protocol_version = rpc::PT_PROTOCOL_VERSION,
            .solc_version = "0.8.30",
            .pt_revision = "test",
            .owner = hexAddress(0x77)
        };
    }

    // Runner.gen's return value: Particles[] with Particles = (string path, uint32[] data).
    inline std::string encodeParticlesResult(
        const std::vector<std::pair<std::string, std::vector<std::uint32_t>>> & particles)
    {
        std::vector<pt::abi::Component> elements;
        for(const auto & [path, values] : particles)
        {
            elements.push_back(pt::abi::Component{
                .dynamic = true,
                .data = pt::abi::encodeTuple({pt::abi::stringComponent(path), pt::abi::uint32ArrayComponent(values)})});
        }
        std::vector<std::uint8_t> array = pt::abi::uintComponent(elements.size()).data;
        const std::vector<std::uint8_t> body = pt::abi::encodeTuple(elements);
        array.insert(array.end(), body.begin(), body.end());
        return chain::bytesToHex(pt::abi::encodeTuple({pt::abi::Component{.dynamic = true, .data = std::move(array)}}));
    }

    // Non-indexed data of TransformationAdded(address caller, string name, address entity,
    // address owner, uint32 argsCount, bytes32 contentHash, bytes32 metadataHash,
    // bytes32 runtimeCodeHash).
    inline std::string encodeTransformationAddedData(
        const std::string & name,
        const chain::Address & owner,
        const evmc::bytes32 & content_hash,
        const evmc::bytes32 & metadata_hash)
    {
        return chain::bytesToHex(pt::abi::encodeTuple({
            pt::abi::addressComponent(makeAddressFromByte(0x41)),
            pt::abi::stringComponent(name),
            pt::abi::addressComponent(makeAddressFromByte(0x55)),
            pt::abi::addressComponent(owner),
            pt::abi::uintComponent(1),
            pt::abi::bytes32Component(content_hash),
            pt::abi::bytes32Component(metadata_hash),
            pt::abi::bytes32Component(bytes32OfFill(0xDD))}));
    }

    // Key of the entity owner in relayed-publication tests.
    inline const std::string OWNER_KEY =
        "0x5151515151515151515151515151515151515151515151515151515151515151";

    inline std::shared_ptr<publish::Signer> makeOwnerSigner()
    {
        const std::filesystem::path key_path = buildPath() / "fake_eth_owner.key";
        std::ofstream out(key_path, std::ios::trunc);
        out << OWNER_KEY;
        out.close();
        // Key files readable by anyone but their owner are refused.
        std::filesystem::permissions(
            key_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);

        auto signer = publish::Signer::fromKeyFile(key_path);
        if(!signer) throw std::runtime_error(signer.error());
        return *signer;
    }

    // A transformation publication `owner` authorizes, with the metadata hash the API
    // derives from the kind and name.
    inline pt::Publication makeRelayedPublication(const chain::Address & owner, const std::string & name)
    {
        pt::Publication publication;
        publication.kind = pt::EntityKind::TRANSFORMATION;
        publication.name = name;
        publication.owner = owner;
        publication.content_hash = bytes32OfFill(0xAA);
        publication.metadata_hash = pt::computeMetadataHash(pt::EntityKind::TRANSFORMATION, name);
        publication.creation_code_hash = bytes32OfFill(0xCC);
        publication.runtime_code_hash = bytes32OfFill(0xDD);
        publication.nonce = 3;
        publication.deadline = 1'900'000'000;
        return publication;
    }

    // The registry call publishing `publication`, as an owner signs it for relay.
    inline publish::Transaction makePublicationTransaction(const pt::Publication & publication)
    {
        return publish::Transaction{
            .chain_id = CHAIN_ID,
            .nonce = 7,
            .max_priority_fee_per_gas = 1'500'000'000,
            .max_fee_per_gas = 3'500'000'000,
            .gas_limit = 200'000,
            .to = evmc::from_hex<chain::Address>(REGISTRY_ADDRESS).value(),
            .value = 0,
            .data = pt::encodePublishTransformationCall(
                std::vector<std::uint8_t>{0x60, 0x00, 0x60, 0x00, 0xF3}, publication, {})};
    }

    inline std::string signTransaction(const publish::Signer & signer, const publish::Transaction & transaction)
    {
        auto raw = signer.signTransaction(transaction);
        if(!raw) throw std::runtime_error(raw.error());
        return *raw;
    }

    // A receipt whose only log is a TransformationAdded event for an entity at 0x55.
    inline json makeTransformationAddedReceipt(
        const std::string & tx_hash,
        const std::string & name,
        const chain::Address & owner,
        const evmc::bytes32 & content_hash,
        const evmc::bytes32 & metadata_hash,
        const std::string & log_address = REGISTRY_ADDRESS)
    {
        return json{
            {"status", "0x1"},
            {"transactionHash", tx_hash},
            {"blockNumber", chain::toHexQuantity(99)},
            {"blockHash", hexBytes(0x41, 32)},
            {"gasUsed", "0x5208"},
            {"logs", json::array({json{
                {"blockNumber", chain::toHexQuantity(99)},
                {"blockHash", hexBytes(0x41, 32)},
                {"transactionHash", tx_hash},
                {"transactionIndex", "0x0"},
                {"logIndex", "0x0"},
                {"address", log_address},
                {"data", encodeTransformationAddedData(name, owner, content_hash, metadata_hash)},
                {"topics", json::array({topicForEvent(
                    "TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)")})},
                {"removed", false}
            }})}
        };
    }
}
