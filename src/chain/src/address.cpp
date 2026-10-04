#include <memory>

#include "address.hpp"
#include "crypto.hpp"
#include "hex.hpp"

namespace dcn::chain
{
    std::optional<chain::Address> readAddressWord(const std::uint8_t* data, std::size_t data_size, std::size_t offset)
    {
        if(data == nullptr || offset > data_size || data_size - offset < 32)
        {
            return std::nullopt;
        }

        // ABI encodes an address as a word whose upper 12 bytes are zero.
        for(std::size_t i = 0; i < 12; ++i)
        {
            if(data[offset + i] != 0)
            {
                return std::nullopt;
            }
        }

        chain::Address addr{};
        std::memcpy(addr.bytes, data + offset + 12, 20);
        return addr;
    }

    std::optional<chain::Address> readAddressWord(const std::vector<std::uint8_t> & data, std::size_t offset)
    {
        return readAddressWord(data.data(), data.size(), offset);
    }

    chain::Address topicWordToAddress(const evmc::bytes32 & topic_word)
    {
        chain::Address addr{};
        std::memcpy(addr.bytes, topic_word.bytes + 12, 20);
        return addr;
    }

    chain::Address ethAddressFromPublicKey(const std::uint8_t* pubkey, std::size_t len) 
    {
        uint8_t hash[crypto::Keccak256::HASH_LEN];
        // skip 0x04 prefix
        dcn::crypto::Keccak256::getHash(pubkey + 1, len - 1, hash);
        chain::Address address;
        // last 20 bytes
        std::copy(hash + 12, hash + 32, address.bytes);
        return address; 
    }

    std::string addressToHex(const chain::Address & address)
    {
        return normalizeHex(evmc::hex(address));
    }

    std::optional<chain::Address> addressFromHex(std::string_view hex)
    {
        if(hex.starts_with("0x")) hex.remove_prefix(2);
        if(hex.size() != 2 * sizeof(chain::Address::bytes)) return std::nullopt;
        return evmc::from_hex<chain::Address>(hex);
    }

    std::string addressToChecksumHex(const chain::Address & address)
    {
        // A letter is upper case where the matching nibble of keccak256(lower-case hex) is 8 or more.
        std::string hex = evmc::hex(address);
        std::uint8_t hash[crypto::Keccak256::HASH_LEN];
        crypto::Keccak256::getHash(reinterpret_cast<const std::uint8_t *>(hex.data()), hex.size(), hash);

        for(std::size_t i = 0; i < hex.size(); ++i)
        {
            const std::uint8_t nibble = (i % 2 == 0) ? (hash[i / 2] >> 4) : (hash[i / 2] & 0x0F);
            if(nibble >= 8 && hex[i] >= 'a' && hex[i] <= 'f') hex[i] = static_cast<char>(hex[i] - 'a' + 'A');
        }
        return "0x" + hex;
    }

}
