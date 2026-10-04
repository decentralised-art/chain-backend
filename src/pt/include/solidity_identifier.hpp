#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace dcn::pt
{
    bool isValidSolidityIdentifier(std::string_view value);

    /**
     * @brief The argument count a transformation or condition body needs: one past the
     * highest index it reads as `args[N]`.
     *
     * @return nullopt when an index does not leave room for a uint32 count.
     */
    std::optional<std::uint32_t> countUsedArgs(std::string_view sol_src);
}
