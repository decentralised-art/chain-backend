#include "solidity_identifier.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace dcn::pt
{
    namespace
    {
        bool isSizedIntegerType(std::string_view value, std::string_view prefix)
        {
            if(!value.starts_with(prefix))
            {
                return false;
            }

            const std::string_view suffix = value.substr(prefix.size());
            if(suffix.empty())
            {
                return false;
            }

            int bits = 0;
            const auto [ptr, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), bits);
            if(ec != std::errc{} || ptr != (suffix.data() + suffix.size()))
            {
                return false;
            }

            return (bits >= 8) && (bits <= 256) && ((bits % 8) == 0);
        }

        bool isSizedBytesType(std::string_view value)
        {
            constexpr std::string_view prefix = "bytes";
            if(!value.starts_with(prefix))
            {
                return false;
            }

            const std::string_view suffix = value.substr(prefix.size());
            if(suffix.empty())
            {
                return false;
            }

            int width = 0;
            const auto [ptr, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), width);
            if(ec != std::errc{} || ptr != (suffix.data() + suffix.size()))
            {
                return false;
            }

            return (width >= 1) && (width <= 32);
        }

        bool isReservedSolidityWord(std::string_view value)
        {
            static const std::unordered_set<std::string_view> reserved_words = {
                // Solidity keywords and contextual keywords.
                "abstract", "after", "alias", "apply", "auto", "break", "case", "catch", "constant",
                "continue", "contract", "default", "define", "delete", "do", "else", "enum", "event",
                "external", "false", "final", "for", "from", "function", "if", "immutable", "implements",
                "import", "in", "indexed", "inline", "interface", "internal", "is", "let", "library",
                "mapping", "memory", "modifier", "mutable", "new", "null", "of", "override", "partial",
                "payable", "pragma", "private", "promise", "public", "pure", "reference", "relocatable",
                "return", "returns", "sealed", "solidity", "static", "storage", "struct", "supports",
                "switch", "true", "try", "type", "typedef", "typeof", "unchecked", "using", "var",
                "view", "virtual", "while",

                // Special function names and error-handling identifiers.
                "constructor", "fallback", "receive", "revert", "require", "assert", "error",

                // Built-in global namespace identifiers.
                "block", "msg", "tx",

                // Inline assembly keyword.
                "assembly",

                // Common built-in value/type names that should not be contract identifiers.
                "address", "bool", "byte", "bytes", "fixed", "int", "string", "ufixed", "uint"
            };

            if(reserved_words.contains(value))
            {
                return true;
            }

            // Reject sized primitive type names, e.g. uint256 / int32 / bytes32.
            return isSizedIntegerType(value, "uint") ||
                   isSizedIntegerType(value, "int") ||
                   isSizedBytesType(value);
        }
    }

    bool isValidSolidityIdentifier(std::string_view value)
    {
        if(value.empty())
        {
            return false;
        }

        const auto is_alpha = [](char ch)
        {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        };

        const auto is_digit = [](char ch)
        {
            return ch >= '0' && ch <= '9';
        };

        const char first = value.front();
        if(!(is_alpha(first) || first == '_'))
        {
            return false;
        }

        for(const char ch : value)
        {
            if(!(is_alpha(ch) || is_digit(ch) || ch == '_'))
            {
                return false;
            }
        }

        if(isReservedSolidityWord(value))
        {
            return false;
        }

        return true;
    }

    std::optional<std::uint32_t> countUsedArgs(const std::string_view sol_src)
    {
        // A plain scan: user source reaches this, and std::regex recurses per character.
        constexpr std::string_view open = "args[";
        std::uint32_t argc = 0;

        for(std::size_t pos = sol_src.find(open); pos != std::string_view::npos; pos = sol_src.find(open, pos))
        {
            pos += open.size();
            const std::size_t digits_end = std::min(sol_src.find_first_not_of("0123456789", pos), sol_src.size());
            if(digits_end == pos || digits_end == sol_src.size() || sol_src[digits_end] != ']')
            {
                continue;
            }

            std::uint64_t index = 0;
            const auto [ptr, ec] = std::from_chars(sol_src.data() + pos, sol_src.data() + digits_end, index);
            if(ec != std::errc{} || index >= std::numeric_limits<std::uint32_t>::max())
            {
                return std::nullopt;
            }

            argc = std::max(argc, static_cast<std::uint32_t>(index) + 1U);
            pos = digits_end + 1;
        }

        return argc;
    }
}
