#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dcn::pt
{
    /// Entity kind ordinals; they must match IRegistry.EntityKind in submodule/pt.
    enum class EntityKind : std::uint8_t
    {
        CONNECTOR = 0,
        TRANSFORMATION = 1,
        CONDITION = 2
    };

    /// "connector", "transformation" or "condition": the form used in artifacts, events and storage.
    std::string_view entityKindName(EntityKind kind);
    std::optional<EntityKind> parseEntityKind(std::string_view name);

    constexpr std::size_t MAX_ENTITY_NAME_LENGTH = 128;

    /// A name this server can handle: it becomes a Solidity contract identifier and a file
    /// name, so only ASCII identifiers up to MAX_ENTITY_NAME_LENGTH characters qualify.
    /// Names from HTTP requests and from chain events are both checked against this rule.
    bool isValidEntityName(std::string_view name);
}
