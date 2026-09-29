#include <algorithm>
#include <array>

#include "chain.hpp"
#include "crypto.hpp"
#include "publication.hpp"

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

        struct RlpItem
        {
            bool is_list = false;
            std::span<const std::uint8_t> payload;
        };

        // The item at the front of `input`; `consumed` receives its encoded size. Only
        // bounds are checked here: canonical form is proven by re-encoding.
        std::optional<RlpItem> readItem(const std::span<const std::uint8_t> input, std::size_t & consumed)
        {
            if(input.empty()) return std::nullopt;

            const std::uint8_t prefix = input[0];
            if(prefix < 0x80)
            {
                consumed = 1;
                return RlpItem{.is_list = false, .payload = input.first(1)};
            }

            const bool is_list = prefix >= 0xC0;
            std::size_t length = prefix - (is_list ? 0xC0 : 0x80);
            std::size_t header = 1;
            if(length > 55)
            {
                const std::size_t length_of_length = length - 55;
                if(length_of_length > sizeof(std::size_t) || input.size() < 1 + length_of_length)
                {
                    return std::nullopt;
                }
                length = 0;
                for(std::size_t i = 0; i < length_of_length; ++i)
                {
                    length = (length << 8) | input[1 + i];
                }
                header += length_of_length;
            }

            if(length > input.size() - header) return std::nullopt;
            consumed = header + length;
            return RlpItem{.is_list = is_list, .payload = input.subspan(header, length)};
        }

        std::optional<std::vector<RlpItem>> readListItems(std::span<const std::uint8_t> payload)
        {
            std::vector<RlpItem> items;
            while(!payload.empty())
            {
                std::size_t consumed = 0;
                const auto item = readItem(payload, consumed);
                if(!item) return std::nullopt;
                items.push_back(*item);
                payload = payload.subspan(consumed);
            }
            return items;
        }

        std::optional<std::uint64_t> readUint(const RlpItem & item)
        {
            if(item.is_list || item.payload.size() > 8) return std::nullopt;
            std::uint64_t value = 0;
            for(const std::uint8_t byte : item.payload)
            {
                value = (value << 8) | byte;
            }
            return value;
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

    std::expected<SignedTransaction, std::string> decodeSignedTransaction(const std::string_view raw_tx_hex)
    {
        const auto raw = evmc::from_hex(std::string(raw_tx_hex));
        if(!raw || raw->empty())
        {
            return std::unexpected("not a hex-encoded transaction");
        }
        if((*raw)[0] != 0x02)
        {
            return std::unexpected("only type-2 (EIP-1559) transactions are accepted");
        }

        const std::span<const std::uint8_t> body(raw->data() + 1, raw->size() - 1);
        std::size_t consumed = 0;
        const auto envelope = readItem(body, consumed);
        if(!envelope || !envelope->is_list || consumed != body.size())
        {
            return std::unexpected("malformed RLP envelope");
        }

        const auto fields = readListItems(envelope->payload);
        if(!fields || fields->size() != 12)
        {
            return std::unexpected("a signed type-2 transaction has twelve fields");
        }
        const std::vector<RlpItem> & field = *fields;

        const auto chain_id = readUint(field[0]);
        const auto nonce = readUint(field[1]);
        const auto max_priority_fee_per_gas = readUint(field[2]);
        const auto max_fee_per_gas = readUint(field[3]);
        const auto gas_limit = readUint(field[4]);
        const auto value = readUint(field[6]);
        if(!chain_id || !nonce || !max_priority_fee_per_gas || !max_fee_per_gas || !gas_limit || !value)
        {
            return std::unexpected("transaction quantities must fit in 64 bits");
        }

        if(field[5].is_list || field[5].payload.size() != 20)
        {
            return std::unexpected("the transaction must call a contract address");
        }
        chain::Address to{};
        std::ranges::copy(field[5].payload, to.bytes);

        if(field[7].is_list)
        {
            return std::unexpected("transaction data must be a byte string");
        }
        if(!field[8].is_list || !field[8].payload.empty())
        {
            return std::unexpected("transactions with an access list are not accepted");
        }

        const auto parity = readUint(field[9]);
        if(!parity || *parity > 1)
        {
            return std::unexpected("signature yParity must be 0 or 1");
        }
        if(field[10].is_list || field[11].is_list || field[10].payload.size() > 32 || field[11].payload.size() > 32)
        {
            return std::unexpected("malformed signature");
        }

        Transaction transaction{
            .chain_id = *chain_id,
            .nonce = *nonce,
            .max_priority_fee_per_gas = *max_priority_fee_per_gas,
            .max_fee_per_gas = *max_fee_per_gas,
            .gas_limit = *gas_limit,
            .to = to,
            .value = *value,
            .data = std::vector<std::uint8_t>(field[7].payload.begin(), field[7].payload.end())
        };

        // r and s right-aligned into 32-byte words, then v as the recovery id.
        std::vector<std::uint8_t> signature(65, 0);
        std::ranges::copy(field[10].payload, signature.begin() + static_cast<std::ptrdiff_t>(32 - field[10].payload.size()));
        std::ranges::copy(field[11].payload, signature.begin() + static_cast<std::ptrdiff_t>(64 - field[11].payload.size()));
        signature[64] = static_cast<std::uint8_t>(*parity);

        // The encoder emits the one canonical form, so reproducing the input byte for byte
        // rejects non-minimal lengths and integers and anything the reader tolerated.
        const auto reencoded = evmc::from_hex(encodeSignedTransaction(transaction, signature));
        if(!reencoded || *reencoded != *raw)
        {
            return std::unexpected("the transaction is not canonically encoded");
        }

        const auto sender = pt::recoverSigner(signingHash(transaction), signature);
        if(!sender)
        {
            return std::unexpected("the signature does not recover to an account");
        }

        return SignedTransaction{
            .transaction = std::move(transaction),
            .sender = *sender,
            .tx_hash = chain::bytes32ToHex(chain::keccakBytes(raw->data(), raw->size()))
        };
    }
}
