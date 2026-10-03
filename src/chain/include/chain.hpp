#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "address.hpp"
#include "deploy.hpp"
#include "execute.hpp"
#include "format_hash.hpp"
#include "hex.hpp"

namespace dcn::chain
{
    std::vector<std::uint8_t> constructSelector(std::string signature);

    evmc::bytes32 constructEventTopic(std::string signature);

    std::optional<std::vector<evmc::bytes32>> decodeTopicWords(const std::vector<std::string> & topics_hex);


    // ABI uint256 word that fits in 64 bits; larger values and short input are rejected.
    std::optional<std::uint64_t> readUint64Word(const std::uint8_t* data, std::size_t data_size, std::size_t offset);
    std::optional<std::uint64_t> readUint64Word(const std::vector<std::uint8_t> & data, std::size_t offset = 0);

    std::optional<std::size_t> readWordAsSizeT(const std::uint8_t* data, std::size_t data_size, std::size_t offset);

    std::optional<std::uint32_t> readUint32Word(const std::uint8_t* data, std::size_t data_size, std::size_t offset);


    std::optional<std::string> decodeAbiString(const std::uint8_t* data, std::size_t data_size, std::size_t string_offset);

    std::optional<std::vector<std::string>> decodeAbiStringArray(const std::uint8_t* data, std::size_t data_size, std::size_t array_offset);

    std::optional<std::vector<std::uint32_t>> decodeAbiUint32Array(const std::uint8_t* data, std::size_t data_size, std::size_t array_offset);

    std::optional<std::vector<std::int32_t>> decodeAbiInt32Array(const std::uint8_t* data, std::size_t data_size, std::size_t array_offset);

    /// The reason string of a Solidity `Error(string)` revert; nullopt for any other revert data.
    std::optional<std::string> decodeRevertReason(std::span<const std::uint8_t> revert_data);
}