#pragma once

#include <string>
#include <string_view>

#include "entity.hpp"

namespace dcn::pt
{
    // Canonical ABI signatures of the events a PT registry proxy emits. The single
    // definition every decoder, log filter and receipt check derives its topic from.

    constexpr std::string_view CONNECTOR_ADDED_SIGNATURE =
        "ConnectorAdded(address,address,string,address,uint32,uint32[],string[],uint32[],uint32[],string[],"
        "string,int32[],bytes32,uint32[],uint32[],uint32[],uint32[],string[],uint32[],int32[],bytes32,bytes32,bytes32)";
    constexpr std::string_view TRANSFORMATION_ADDED_SIGNATURE =
        "TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)";
    constexpr std::string_view CONDITION_ADDED_SIGNATURE =
        "ConditionAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)";

    constexpr std::string_view CONNECTOR_REMOVED_SIGNATURE =
        "ConnectorRemoved(address,address,string,address,bytes32,bytes32)";
    constexpr std::string_view TRANSFORMATION_REMOVED_SIGNATURE =
        "TransformationRemoved(address,address,string,address,bytes32,bytes32)";
    constexpr std::string_view CONDITION_REMOVED_SIGNATURE =
        "ConditionRemoved(address,address,string,address,bytes32,bytes32)";

    constexpr std::string_view ENTITY_OWNER_CHANGED_SIGNATURE =
        "EntityOwnerChanged(uint8,string,address,address,address)";

    // ERC-1967 proxies emit this when their implementation changes.
    constexpr std::string_view PROXY_UPGRADED_SIGNATURE = "Upgraded(address)";

    std::string_view addedEventSignature(EntityKind kind);

    // 0x-prefixed lowercase keccak256 of an event signature: the log's topic0.
    std::string eventTopic(std::string_view signature);
}
