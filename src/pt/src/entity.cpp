#include "entity.hpp"

#include <algorithm>

namespace dcn::pt
{
    std::string_view entityKindName(const EntityKind kind)
    {
        switch(kind)
        {
        case EntityKind::CONNECTOR: return "connector";
        case EntityKind::TRANSFORMATION: return "transformation";
        case EntityKind::CONDITION: return "condition";
        }
        return "unknown";
    }

    std::optional<EntityKind> parseEntityKind(const std::string_view name)
    {
        if(name == "connector") return EntityKind::CONNECTOR;
        if(name == "transformation") return EntityKind::TRANSFORMATION;
        if(name == "condition") return EntityKind::CONDITION;
        return std::nullopt;
    }

    bool isValidEntityName(const std::string_view name)
    {
        if(name.empty() || name.size() > MAX_ENTITY_NAME_LENGTH)
        {
            return false;
        }

        const auto is_letter = [](const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        const auto is_digit = [](const char c) { return c >= '0' && c <= '9'; };

        if(!is_letter(name.front()) && name.front() != '_')
        {
            return false;
        }

        return std::ranges::all_of(name, [&](const char c) { return is_letter(c) || is_digit(c) || c == '_'; });
    }
}
