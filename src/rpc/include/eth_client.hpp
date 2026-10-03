#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio.hpp>

#include "json_rpc.hpp"

namespace dcn::rpc
{
    constexpr std::string_view BLOCK_TAG_LATEST = "latest";
    constexpr std::string_view BLOCK_TAG_SAFE = "safe";
    constexpr std::string_view BLOCK_TAG_FINALIZED = "finalized";
    constexpr std::string_view BLOCK_TAG_PENDING = "pending";

    struct BlockHeader
    {
        std::int64_t number = 0;
        std::string hash;
        std::string parent_hash;
        std::int64_t timestamp = 0;
    };

    // One mined transaction outcome.
    struct TransactionReceipt
    {
        bool success = false;
        std::string tx_hash;
        std::int64_t block_number = 0;
        std::string block_hash;
        std::string contract_address;   // empty unless the transaction created a contract
        std::uint64_t gas_used = 0;
        std::vector<json> logs;
    };

    // A transaction as eth_getTransactionByHash reports it.
    struct TransactionInfo
    {
        std::string tx_hash;
        std::string block_hash;   // empty while the transaction is pending
        std::string input;
    };

    struct FeeEstimate
    {
        std::uint64_t max_fee_per_gas = 0;
        std::uint64_t max_priority_fee_per_gas = 0;
        std::uint64_t base_fee_per_gas = 0;
    };

    struct LogFilter
    {
        std::int64_t from_block = 0;
        std::int64_t to_block = 0;
        std::string address;
        std::vector<std::string> topic0;
    };

    // Typed wrapper over the standard Ethereum JSON-RPC surface. Every response is
    // treated as untrusted: quantities are range-checked and data fields are only
    // accepted as hex strings.
    class EthClient
    {
        public:
            explicit EthClient(std::shared_ptr<JsonRpcClient> client);

            asio::awaitable<std::expected<std::uint64_t, RpcError>> chainId() const;
            asio::awaitable<std::expected<std::int64_t, RpcError>> blockNumber() const;

            // nullopt when the node does not know the block (or, for a tag, does not serve it).
            asio::awaitable<std::expected<std::optional<BlockHeader>, RpcError>> blockByTag(
                std::string_view tag) const;
            asio::awaitable<std::expected<std::optional<BlockHeader>, RpcError>> blockByNumber(
                std::int64_t block_number) const;

            asio::awaitable<std::expected<std::string, RpcError>> getCode(
                std::string_view address,
                std::string_view tag = BLOCK_TAG_LATEST) const;

            // getCode() at exactly the block with `block_hash` (EIP-1898).
            asio::awaitable<std::expected<std::string, RpcError>> getCodeAtBlock(
                std::string_view address,
                std::string_view block_hash) const;

            // A timed-out range is reported rather than retried: it needs a narrower range.
            asio::awaitable<std::expected<std::vector<json>, RpcError>> getLogs(const LogFilter & filter) const;

            // nullopt when the node does not know the transaction.
            asio::awaitable<std::expected<std::optional<TransactionInfo>, RpcError>> transactionByHash(
                std::string_view tx_hash) const;

            // `gas` bounds the call the way a transaction would; without it the provider
            // applies its own ceiling.
            asio::awaitable<std::expected<std::string, RpcError>> call(
                std::string_view to,
                std::string_view data_hex,
                std::string_view tag = BLOCK_TAG_LATEST,
                std::optional<std::uint64_t> gas = std::nullopt) const;

            // call() evaluated at exactly the block with `block_hash` (EIP-1898): a reorg that
            // replaces the block at that height cannot change which state answered.
            asio::awaitable<std::expected<std::string, RpcError>> callAtBlock(
                std::string_view to,
                std::string_view data_hex,
                std::string_view block_hash,
                std::optional<std::uint64_t> gas = std::nullopt) const;

            asio::awaitable<std::expected<std::string, RpcError>> getStorageAt(
                std::string_view address,
                std::string_view slot_hex,
                std::string_view tag = BLOCK_TAG_LATEST) const;

            asio::awaitable<std::expected<std::uint64_t, RpcError>> transactionCount(
                std::string_view address,
                std::string_view tag) const;

            asio::awaitable<std::expected<std::uint64_t, RpcError>> estimateGas(
                std::string_view from,
                std::optional<std::string_view> to,
                std::string_view data_hex) const;

            // EIP-1559 fees from the head block's base fee. Fails on a chain or provider that
            // does not report a base fee rather than signing an underpriced transaction.
            asio::awaitable<std::expected<FeeEstimate, RpcError>> feeEstimate() const;

            asio::awaitable<std::expected<std::string, RpcError>> sendRawTransaction(
                std::string_view raw_tx_hex) const;

            // nullopt while the transaction is still unmined, which is not an error.
            asio::awaitable<std::expected<std::optional<TransactionReceipt>, RpcError>> transactionReceipt(
                std::string_view tx_hash) const;

            std::string_view endpoint() const;

            // Aborts in-flight requests and fails later ones; used on shutdown.
            void cancel() const;

        private:
            std::shared_ptr<JsonRpcClient> _client;
    };

    // ERC-1967 implementation slot: keccak256("eip1967.proxy.implementation") - 1.
    constexpr std::string_view ERC1967_IMPLEMENTATION_SLOT =
        "0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc";

    // Reads the implementation address an ERC-1967 proxy delegates to.
    asio::awaitable<std::expected<std::string, RpcError>> readProxyImplementation(
        const EthClient & client,
        std::string_view proxy_address);

    // keccak256 of the code deployed at `address`, or nullopt when there is no code.
    asio::awaitable<std::expected<std::optional<std::string>, RpcError>> readCodeHash(
        const EthClient & client,
        std::string_view address);

    // An address returned as the right-most 20 bytes of one ABI or storage word.
    std::expected<std::string, RpcError> addressFromWord(const std::string & word_hex, std::string_view field);

    // An unsigned integer returned as one ABI word; rejects values above 64 bits.
    std::expected<std::uint64_t, RpcError> uint64FromWord(const std::string & word_hex, std::string_view field);
}
