#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
#include <string_view>
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
    struct ScalarHashEntry
    {
        evmc::bytes32 scalar_hash;
        evmc::bytes32 path_hash;
    };

    struct ScalarLabel
    {
        std::string scalar;
        evmc::bytes32 path_hash;
        std::uint32_t tail_id;
    };

    struct ResolvedScalarEntries
    {
        std::vector<ScalarHashEntry> hash_entries;
        std::vector<ScalarLabel> display_entries;
    };

    bool equalBytes32(const evmc::bytes32 & lhs, const evmc::bytes32 & rhs);

    bool lessBytes32(const evmc::bytes32 & lhs, const evmc::bytes32 & rhs);

    evmc::bytes32 keccakBytes(const std::uint8_t * data, std::size_t size);

    evmc::bytes32 keccakBytes(std::span<const std::uint8_t> bytes);

    evmc::bytes32 keccakString(std::string_view value);

    evmc::bytes32 composeFormatHash(const evmc::bytes32 & lhs, const evmc::bytes32 & rhs);

    evmc::bytes32 dimPathHash(std::uint32_t dim_id);

    evmc::bytes32 concatPathHash(const evmc::bytes32 & left, const evmc::bytes32 & right);

    evmc::bytes32 scalarPathLabelHash(const evmc::bytes32 & scalar_hash, const evmc::bytes32 & path_hash);

    evmc::bytes32 labelHashToFormatHash(const evmc::bytes32 & label_hash);

    evmc::bytes32 computeFormatHash(const std::vector<ScalarHashEntry> & hash_entries);

    std::vector<ScalarLabel> canonicalizeScalarLabels(const std::vector<ScalarLabel> & labels);

    bool scalarLabelsEqual(const std::vector<ScalarLabel> & lhs, const std::vector<ScalarLabel> & rhs);
}
