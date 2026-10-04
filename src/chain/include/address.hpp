#pragma once

#include <optional>
#include <cstdint>
#include <vector>


#ifdef interface
    #undef interface
#endif
#include <evmc/evmc.hpp>
#ifndef interface
    #define interface __STRUCT__
#endif

namespace dcn::chain
{
    using Address = evmc::address;

    std::optional<chain::Address> readAddressWord(const std::uint8_t* data, std::size_t data_size, std::size_t offset = 0);
    std::optional<chain::Address> readAddressWord(const std::vector<std::uint8_t> & data, std::size_t offset = 0);

    chain::Address topicWordToAddress(const evmc::bytes32 & topic_word);

    // Get Ethereum address from public key (last 20 bytes of Keccak256(pubkey))
    chain::Address ethAddressFromPublicKey(const std::uint8_t* pubkey, std::size_t len);

    std::string addressToHex(const chain::Address & address);

    /// `0x`-prefixed hex with the EIP-55 mixed-case checksum.
    std::string addressToChecksumHex(const chain::Address & address);

}
