#include <algorithm>
#include <array>

#include "chain.hpp"
#include "crypto.hpp"

#include "eth_tx.hpp"

namespace dcn::publish
{
    namespace
    {
        std::vector<std::uint8_t> encodeLength(const std::size_t length, const std::uint8_t offset)
        {
            if(length < 56)
            {
                return {static_cast<std::uint8_t>(offset + length)};
            }

            // Big-endian minimal length prefix.
            std::vector<std::uint8_t> length_bytes;
            for(std::size_t value = length; value != 0; value >>= 8)
            {
                length_bytes.insert(length_bytes.begin(), static_cast<std::uint8_t>(value & 0xFFu));
            }

            std::vector<std::uint8_t> out{static_cast<std::uint8_t>(offset + 55 + length_bytes.size())};
            out.insert(out.end(), length_bytes.begin(), length_bytes.end());
            return out;
        }

        std::vector<std::uint8_t> minimalBigEndian(const std::uint64_t value)
        {
            std::vector<std::uint8_t> out;
            for(std::uint64_t remaining = value; remaining != 0; remaining >>= 8)
            {
                out.insert(out.begin(), static_cast<std::uint8_t>(remaining & 0xFFu));
            }
            return out;
        }

        evmc::bytes32 keccak(std::span<const std::uint8_t> bytes)
        {
            return chain::keccakBytes(bytes.data(), bytes.size());
        }

        // The nine fields every type-2 transaction carries, signed and unsigned alike.
        std::vector<std::vector<std::uint8_t>> payloadFields(const Transaction & transaction)
        {
            std::vector<std::vector<std::uint8_t>> fields;
            fields.push_back(rlp::encodeUint(transaction.chain_id));
            fields.push_back(rlp::encodeUint(transaction.nonce));
            fields.push_back(rlp::encodeUint(transaction.max_priority_fee_per_gas));
            fields.push_back(rlp::encodeUint(transaction.max_fee_per_gas));
            fields.push_back(rlp::encodeUint(transaction.gas_limit));
            fields.push_back(transaction.to
                ? rlp::encodeBytes(std::span<const std::uint8_t>(transaction.to->bytes, 20))
                : rlp::encodeBytes({}));
            fields.push_back(rlp::encodeUint(transaction.value));
            fields.push_back(rlp::encodeBytes(transaction.data));
            // Empty access list.
            fields.push_back(rlp::encodeList({}));
            return fields;
        }

    }

    namespace rlp
    {
        std::vector<std::uint8_t> encodeBytes(const std::span<const std::uint8_t> value)
        {
            if(value.size() == 1 && value[0] < 0x80)
            {
                return {value[0]};
            }

            std::vector<std::uint8_t> out = encodeLength(value.size(), 0x80);
            out.insert(out.end(), value.begin(), value.end());
            return out;
        }

        std::vector<std::uint8_t> encodeUint(const std::uint64_t value)
        {
            // RLP integers are minimal big-endian; zero is the empty string.
            const std::vector<std::uint8_t> bytes = minimalBigEndian(value);
            return encodeBytes(bytes);
        }

        std::vector<std::uint8_t> encodeList(const std::span<const std::vector<std::uint8_t>> items)
        {
            std::vector<std::uint8_t> payload;
            for(const auto & item : items)
            {
                payload.insert(payload.end(), item.begin(), item.end());
            }

            std::vector<std::uint8_t> out = encodeLength(payload.size(), 0xC0);
            out.insert(out.end(), payload.begin(), payload.end());
            return out;
        }
    }

    evmc::bytes32 signingHash(const Transaction & transaction)
    {
        std::vector<std::uint8_t> preimage{0x02};
        const std::vector<std::uint8_t> body = rlp::encodeList(payloadFields(transaction));
        preimage.insert(preimage.end(), body.begin(), body.end());
        return keccak(preimage);
    }

    std::string encodeSignedTransaction(
        const Transaction & transaction,
        const std::span<const std::uint8_t> signature)
    {
        if(signature.size() != 65)
        {
            return {};
        }

        std::vector<std::vector<std::uint8_t>> fields = payloadFields(transaction);

        // yParity, then r and s with leading zero bytes stripped.
        const std::uint8_t parity = signature[64] >= 27
            ? static_cast<std::uint8_t>(signature[64] - 27)
            : signature[64];
        fields.push_back(rlp::encodeUint(parity));

        const auto strip = [](const std::span<const std::uint8_t> word)
        {
            const auto first = std::ranges::find_if(word, [](const std::uint8_t b) { return b != 0; });
            return std::vector<std::uint8_t>(first, word.end());
        };
        fields.push_back(rlp::encodeBytes(strip(signature.subspan(0, 32))));
        fields.push_back(rlp::encodeBytes(strip(signature.subspan(32, 32))));

        std::vector<std::uint8_t> raw{0x02};
        const std::vector<std::uint8_t> body = rlp::encodeList(fields);
        raw.insert(raw.end(), body.begin(), body.end());

        return chain::bytesToHex(raw);
    }

    std::string transactionHash(const std::string_view raw_tx_hex)
    {
        const auto bytes = evmc::from_hex(std::string(raw_tx_hex));
        if(!bytes) return {};
        return chain::bytes32ToHex(chain::keccakBytes(bytes->data(), bytes->size()));
    }
}
