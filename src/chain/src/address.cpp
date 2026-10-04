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

}
