#include "eth_client.hpp"

#include <algorithm>
#include <format>
#include <limits>

#include "chain.hpp"

namespace dcn::rpc
{
    namespace
    {
        // Hard ceilings on anything a provider can inflate. A hostile or broken
        // endpoint must not be able to make the ingestion loop allocate without bound.
        constexpr std::size_t MAX_LOGS_PER_RESPONSE = 100'000;
        constexpr std::size_t MAX_HEX_DATA_CHARS = 8ull * 1024ull * 1024ull;

        // Fallback tip when the provider does not implement eth_maxPriorityFeePerGas.
        constexpr std::uint64_t DEFAULT_PRIORITY_FEE_WEI = 1'500'000'000;

        RpcError _protocolError(std::string message)
        {
            return RpcError{.kind = RpcError::Kind::PROTOCOL, .code = 0, .message = std::move(message)};
        }

        bool _isHexDigits(const std::string_view value)
        {
            return std::ranges::all_of(value, [](const char c)
            {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            });
        }

        // JSON-RPC QUANTITY: a 0x-prefixed, non-empty hex number. Decimal strings and
        // bare hex are rejected rather than guessed at.
        std::expected<std::int64_t, RpcError> _quantity(const json & value, const std::string_view field)
        {
            if(!value.is_string())
            {
                return std::unexpected(_protocolError(std::format("{} is not a quantity string", field)));
            }

            const std::string text = value.get<std::string>();
            if(text.size() < 3 || !text.starts_with("0x") || !_isHexDigits(std::string_view(text).substr(2)))
            {
                return std::unexpected(_protocolError(std::format("{} is not a 0x-prefixed quantity", field)));
            }

            const auto parsed = parse::parseHexQuantity(text);
            if(!parsed)
            {
                return std::unexpected(_protocolError(std::format("{} is out of range", field)));
            }

            return *parsed;
        }

        // JSON-RPC DATA: 0x-prefixed hex with an even number of digits.
        std::expected<std::string, RpcError> _dataString(const json & value, const std::string_view field)
        {
            if(!value.is_string())
            {
                return std::unexpected(_protocolError(std::format("{} is not a data string", field)));
            }

            std::string out = value.get<std::string>();
            if(out.size() > MAX_HEX_DATA_CHARS)
            {
                return std::unexpected(RpcError{
                    .kind = RpcError::Kind::LIMIT, .code = 0, .message = std::format("{} exceeds size limit", field)});
            }

            const std::string_view digits = std::string_view(out).substr(std::min<std::size_t>(2, out.size()));
            if(!out.starts_with("0x") || (digits.size() % 2) != 0 || !_isHexDigits(digits))
            {
                return std::unexpected(_protocolError(std::format("{} is not 0x-prefixed hex data", field)));
            }

            return out;
        }

        json _member(const json & object, const char * name)
        {
            const auto it = object.find(name);
            return it == object.end() ? json{} : *it;
        }

        std::expected<BlockHeader, RpcError> _parseHeader(const json & block)
        {
            if(!block.is_object())
            {
                return std::unexpected(_protocolError("block is not an object"));
            }

            const auto number = _quantity(_member(block, "number"), "block.number");
            if(!number) return std::unexpected(number.error());

            const auto timestamp = _quantity(_member(block, "timestamp"), "block.timestamp");
            if(!timestamp) return std::unexpected(timestamp.error());

            const auto hash = _dataString(_member(block, "hash"), "block.hash");
            if(!hash) return std::unexpected(hash.error());

            const auto parent_hash = _dataString(_member(block, "parentHash"), "block.parentHash");
            if(!parent_hash) return std::unexpected(parent_hash.error());

            return BlockHeader{
                .number = *number,
                .hash = chain::normalizeHex(*hash),
                .parent_hash = chain::normalizeHex(*parent_hash),
                .timestamp = *timestamp
            };
        }

        std::string _quantityHex(const std::uint64_t value)
        {
            return std::format("0x{:x}", value);
        }

        asio::awaitable<std::expected<std::optional<BlockHeader>, RpcError>> _block(
            const std::shared_ptr<JsonRpcClient> & client,
            std::string block_parameter)
        {
            auto result = co_await client->call("eth_getBlockByNumber", json::array({std::move(block_parameter), false}));
            if(!result) co_return std::unexpected(result.error());

            if(result->is_null())
            {
                co_return std::optional<BlockHeader>{};
            }

            const auto header = _parseHeader(*result);
            if(!header) co_return std::unexpected(header.error());

            co_return std::optional<BlockHeader>{*header};
        }
    }

    std::expected<std::string, RpcError> addressFromWord(const std::string & word_hex, const std::string_view field)
    {
        const auto bytes = evmc::from_hex(word_hex);
        if(!bytes || bytes->size() != 32)
        {
            return std::unexpected(_protocolError(std::format("{} did not return one word", field)));
        }

        const auto address = chain::readAddressWord(bytes->data(), bytes->size(), 0);
        if(!address)
        {
            return std::unexpected(_protocolError(std::format("{} is not an ABI address", field)));
        }
        return chain::addressToHex(*address);
    }

    std::expected<std::uint64_t, RpcError> uint64FromWord(const std::string & word_hex, const std::string_view field)
    {
        const auto bytes = evmc::from_hex(word_hex);
        if(!bytes || bytes->size() != 32)
        {
            return std::unexpected(_protocolError(std::format("{} did not return one word", field)));
        }

        // Silently truncating a larger value would act on a number the contract never held.
        if(std::any_of(bytes->begin(), bytes->begin() + 24, [](const std::uint8_t b) { return b != 0; }))
        {
            return std::unexpected(_protocolError(std::format("{} is out of supported range", field)));
        }

        std::uint64_t value = 0;
        for(std::size_t i = 24; i < 32; ++i)
        {
            value = (value << 8) | (*bytes)[i];
        }
        return value;
    }

    EthClient::EthClient(std::shared_ptr<JsonRpcClient> client)
        : _client(std::move(client))
    {
    }

    std::string_view EthClient::endpoint() const
    {
        return _client ? _client->endpoint() : std::string_view{"<none>"};
    }

    void EthClient::cancel() const
    {
        if(_client) _client->cancel();
    }

    asio::awaitable<std::expected<std::uint64_t, RpcError>> EthClient::chainId() const
    {
        auto result = co_await _client->call("eth_chainId", json::array());
        if(!result) co_return std::unexpected(result.error());

        const auto value = _quantity(*result, "eth_chainId");
        if(!value) co_return std::unexpected(value.error());

        if(*value <= 0)
        {
            co_return std::unexpected(_protocolError("eth_chainId is zero"));
        }

        co_return static_cast<std::uint64_t>(*value);
    }

    asio::awaitable<std::expected<std::int64_t, RpcError>> EthClient::blockNumber() const
    {
        auto result = co_await _client->call("eth_blockNumber", json::array());
        if(!result) co_return std::unexpected(result.error());

        co_return _quantity(*result, "eth_blockNumber");
    }

    asio::awaitable<std::expected<std::optional<BlockHeader>, RpcError>> EthClient::blockByTag(
        const std::string_view tag) const
    {
        co_return co_await _block(_client, std::string(tag));
    }

    asio::awaitable<std::expected<std::optional<BlockHeader>, RpcError>> EthClient::blockByNumber(
        const std::int64_t block_number) const
    {
        co_return co_await _block(_client, chain::toHexQuantity(block_number));
    }

    asio::awaitable<std::expected<std::string, RpcError>> EthClient::getCode(
        const std::string_view address,
        const std::string_view tag) const
    {
        auto result = co_await _client->call(
            "eth_getCode", json::array({std::string(address), std::string(tag)}));
        if(!result) co_return std::unexpected(result.error());

        co_return _dataString(*result, "eth_getCode");
    }

    asio::awaitable<std::expected<std::vector<json>, RpcError>> EthClient::getLogs(const LogFilter & filter) const
    {
        json params{
            {"fromBlock", chain::toHexQuantity(filter.from_block)},
            {"toBlock", chain::toHexQuantity(filter.to_block)}
        };

        if(!filter.address.empty())
        {
            params["address"] = filter.address;
        }

        if(!filter.topic0.empty())
        {
            params["topics"] = json::array({filter.topic0});
        }

        auto result = co_await _client->call("eth_getLogs", json::array({std::move(params)}), false);
        if(!result) co_return std::unexpected(result.error());

        if(!result->is_array())
        {
            co_return std::unexpected(_protocolError("eth_getLogs result is not an array"));
        }

        if(result->size() > MAX_LOGS_PER_RESPONSE)
        {
            co_return std::unexpected(RpcError{
                .kind = RpcError::Kind::LIMIT,
                .code = 0,
                .message = "eth_getLogs returned more logs than the configured limit"});
        }

        std::vector<json> logs;
        logs.reserve(result->size());
        for(auto & entry : *result)
        {
            logs.push_back(std::move(entry));
        }

        co_return logs;
    }

    asio::awaitable<std::expected<std::string, RpcError>> EthClient::call(
        const std::string_view to,
        const std::string_view data_hex,
        const std::string_view tag,
        const std::optional<std::uint64_t> gas) const
    {
        json call_object{
            {"to", std::string(to)},
            {"data", std::string(data_hex)}
        };
        if(gas)
        {
            call_object["gas"] = _quantityHex(*gas);
        }

        auto result = co_await _client->call(
            "eth_call", json::array({std::move(call_object), std::string(tag)}));
        if(!result) co_return std::unexpected(result.error());

        co_return _dataString(*result, "eth_call");
    }

    asio::awaitable<std::expected<std::string, RpcError>> EthClient::callAtBlock(
        const std::string_view to,
        const std::string_view data_hex,
        const std::string_view block_hash,
        const std::optional<std::uint64_t> gas) const
    {
        json call_object{
            {"to", std::string(to)},
            {"data", std::string(data_hex)}
        };
        if(gas)
        {
            call_object["gas"] = _quantityHex(*gas);
        }

        auto result = co_await _client->call(
            "eth_call",
            json::array({
                std::move(call_object),
                json{{"blockHash", std::string(block_hash)}, {"requireCanonical", true}}}));
        if(!result) co_return std::unexpected(result.error());

        co_return _dataString(*result, "eth_call");
    }

    asio::awaitable<std::expected<std::string, RpcError>> EthClient::getStorageAt(
        const std::string_view address,
        const std::string_view slot_hex,
        const std::string_view tag) const
    {
        auto result = co_await _client->call(
            "eth_getStorageAt",
            json::array({std::string(address), std::string(slot_hex), std::string(tag)}));
        if(!result) co_return std::unexpected(result.error());

        co_return _dataString(*result, "eth_getStorageAt");
    }

    asio::awaitable<std::expected<std::uint64_t, RpcError>> EthClient::transactionCount(
        const std::string_view address,
        const std::string_view tag) const
    {
        auto result = co_await _client->call(
            "eth_getTransactionCount", json::array({std::string(address), std::string(tag)}));
        if(!result) co_return std::unexpected(result.error());

        const auto value = _quantity(*result, "eth_getTransactionCount");
        if(!value) co_return std::unexpected(value.error());

        co_return static_cast<std::uint64_t>(*value);
    }

    asio::awaitable<std::expected<std::uint64_t, RpcError>> EthClient::estimateGas(
        const std::string_view from,
        const std::optional<std::string_view> to,
        const std::string_view data_hex) const
    {
        json call_object{
            {"from", std::string(from)},
            {"data", std::string(data_hex)}
        };
        if(to) call_object["to"] = std::string(*to);

        auto result = co_await _client->call("eth_estimateGas", json::array({std::move(call_object)}));
        if(!result) co_return std::unexpected(result.error());

        const auto value = _quantity(*result, "eth_estimateGas");
        if(!value) co_return std::unexpected(value.error());

        co_return static_cast<std::uint64_t>(*value);
    }

    asio::awaitable<std::expected<FeeEstimate, RpcError>> EthClient::feeEstimate() const
    {
        // The shared header parser keeps only the fields ingestion needs, so the raw
        // block is read here for baseFeePerGas.
        auto head_block = co_await _client->call(
            "eth_getBlockByNumber", json::array({std::string(BLOCK_TAG_LATEST), false}));
        if(!head_block) co_return std::unexpected(head_block.error());

        if(!head_block->is_object() || !head_block->contains("baseFeePerGas"))
        {
            co_return std::unexpected(_protocolError("head block carries no EIP-1559 base fee"));
        }

        const auto base_fee = _quantity(head_block->at("baseFeePerGas"), "block.baseFeePerGas");
        if(!base_fee) co_return std::unexpected(base_fee.error());

        std::uint64_t priority_fee = DEFAULT_PRIORITY_FEE_WEI;
        auto priority = co_await _client->call("eth_maxPriorityFeePerGas", json::array());
        if(priority)
        {
            const auto parsed = _quantity(*priority, "eth_maxPriorityFeePerGas");
            if(parsed) priority_fee = static_cast<std::uint64_t>(*parsed);
        }
        else if(priority.error().retryable())
        {
            co_return std::unexpected(priority.error());
        }

        // Two base fees of headroom absorbs the 12.5% per-block maximum increase for
        // several blocks without overpaying: the unused base-fee portion is refunded.
        const std::uint64_t base = static_cast<std::uint64_t>(*base_fee);
        co_return FeeEstimate{
            .max_fee_per_gas = base * 2 + priority_fee,
            .max_priority_fee_per_gas = priority_fee,
            .base_fee_per_gas = base
        };
    }

    asio::awaitable<std::expected<std::string, RpcError>> EthClient::sendRawTransaction(
        const std::string_view raw_tx_hex) const
    {
        auto result = co_await _client->call(
            "eth_sendRawTransaction", json::array({std::string(raw_tx_hex)}));
        if(!result) co_return std::unexpected(result.error());

        const auto hash = _dataString(*result, "eth_sendRawTransaction");
        if(!hash) co_return std::unexpected(hash.error());

        co_return chain::normalizeHex(*hash);
    }

    asio::awaitable<std::expected<std::optional<TransactionReceipt>, RpcError>> EthClient::transactionReceipt(
        const std::string_view tx_hash) const
    {
        auto result = co_await _client->call(
            "eth_getTransactionReceipt", json::array({std::string(tx_hash)}));
        if(!result) co_return std::unexpected(result.error());

        if(result->is_null())
        {
            co_return std::optional<TransactionReceipt>{};
        }

        if(!result->is_object())
        {
            co_return std::unexpected(_protocolError("eth_getTransactionReceipt is not an object"));
        }

        const auto status = _quantity(_member(*result, "status"), "receipt.status");
        if(!status) co_return std::unexpected(status.error());

        const auto block_number = _quantity(_member(*result, "blockNumber"), "receipt.blockNumber");
        if(!block_number) co_return std::unexpected(block_number.error());

        const auto block_hash = _dataString(_member(*result, "blockHash"), "receipt.blockHash");
        if(!block_hash) co_return std::unexpected(block_hash.error());

        const auto gas_used = _quantity(_member(*result, "gasUsed"), "receipt.gasUsed");
        if(!gas_used) co_return std::unexpected(gas_used.error());

        const auto receipt_tx_hash = _dataString(_member(*result, "transactionHash"), "receipt.transactionHash");
        if(!receipt_tx_hash) co_return std::unexpected(receipt_tx_hash.error());

        if(chain::normalizeHex(*receipt_tx_hash) != chain::normalizeHex(std::string(tx_hash)))
        {
            co_return std::unexpected(_protocolError("receipt belongs to a different transaction"));
        }

        TransactionReceipt receipt{
            .success = (*status == 1),
            .tx_hash = chain::normalizeHex(*receipt_tx_hash),
            .block_number = *block_number,
            .block_hash = chain::normalizeHex(*block_hash),
            .gas_used = static_cast<std::uint64_t>(*gas_used)
        };

        const json contract_address = _member(*result, "contractAddress");
        if(contract_address.is_string())
        {
            const auto parsed = _dataString(contract_address, "receipt.contractAddress");
            if(!parsed) co_return std::unexpected(parsed.error());
            receipt.contract_address = chain::normalizeHex(*parsed);
        }

        const json logs = _member(*result, "logs");
        if(logs.is_array())
        {
            if(logs.size() > MAX_LOGS_PER_RESPONSE)
            {
                co_return std::unexpected(RpcError{
                    .kind = RpcError::Kind::LIMIT,
                    .code = 0,
                    .message = "receipt carries more logs than the configured limit"});
            }
            receipt.logs.assign(logs.begin(), logs.end());
        }

        co_return std::optional<TransactionReceipt>{std::move(receipt)};
    }

    asio::awaitable<std::expected<std::string, RpcError>> readProxyImplementation(
        const EthClient & client,
        const std::string_view proxy_address)
    {
        auto slot = co_await client.getStorageAt(proxy_address, ERC1967_IMPLEMENTATION_SLOT);
        if(!slot) co_return std::unexpected(slot.error());

        co_return addressFromWord(*slot, "implementation slot");
    }

    asio::awaitable<std::expected<std::optional<std::string>, RpcError>> readCodeHash(
        const EthClient & client,
        const std::string_view address)
    {
        auto code = co_await client.getCode(address);
        if(!code) co_return std::unexpected(code.error());

        const auto bytes = evmc::from_hex(*code);
        if(!bytes) co_return std::unexpected(_protocolError("eth_getCode is not hex data"));
        if(bytes->empty()) co_return std::optional<std::string>{};

        co_return std::optional<std::string>{
            chain::bytes32ToHex(chain::keccakBytes(bytes->data(), bytes->size()))};
    }
}
