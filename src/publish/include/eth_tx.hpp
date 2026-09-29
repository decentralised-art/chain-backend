#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "address.hpp"

namespace dcn::publish
{
    /// Minimal RLP writer: the only shapes an EIP-1559 transaction needs.
    namespace rlp
    {
        std::vector<std::uint8_t> encodeBytes(std::span<const std::uint8_t> value);
        std::vector<std::uint8_t> encodeUint(std::uint64_t value);
        std::vector<std::uint8_t> encodeList(std::span<const std::vector<std::uint8_t>> items);
    }

    /// An EIP-1559 (type 2) transaction before signing. `to` is empty for a contract
    /// creation. Fees and gas are 64-bit: no real publisher reaches 2^64 wei per gas.
    struct Transaction
    {
        std::uint64_t chain_id = 0;
        std::uint64_t nonce = 0;
        std::uint64_t max_priority_fee_per_gas = 0;
        std::uint64_t max_fee_per_gas = 0;
        std::uint64_t gas_limit = 0;
        std::optional<chain::Address> to;
        std::uint64_t value = 0;
        std::vector<std::uint8_t> data;
    };

    /// keccak256(0x02 || rlp(unsigned fields)): what a signing key signs.
    evmc::bytes32 signingHash(const Transaction & transaction);

    /// `0x02 || rlp(fields || yParity || r || s)`, ready for eth_sendRawTransaction.
    /// `signature` is 65 bytes of `r || s || v` as produced by signDigest.
    std::string encodeSignedTransaction(
        const Transaction & transaction,
        std::span<const std::uint8_t> signature);

    /// Transaction hash of a signed transaction, as the provider will report it.
    std::string transactionHash(std::string_view raw_tx_hex);

    /// A signed type-2 transaction and the account whose key signed it.
    struct SignedTransaction
    {
        Transaction transaction;
        chain::Address sender{};
        std::string tx_hash;
    };

    /// Decodes a `0x02 || rlp(...)` transaction and recovers its sender. Only the form
    /// encodeSignedTransaction produces is accepted: a type-2 envelope calling an address,
    /// with an empty access list, 64-bit quantities, canonical RLP and a low-s
    /// signature, so every accepted transaction has exactly one encoding and one hash.
    std::expected<SignedTransaction, std::string> decodeSignedTransaction(std::string_view raw_tx_hex);
}
