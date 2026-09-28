#include "registry_events.hpp"

#include "chain.hpp"

namespace dcn::pt
{
    std::string_view addedEventSignature(const EntityKind kind)
    {
        switch(kind)
        {
        case EntityKind::CONNECTOR: return CONNECTOR_ADDED_SIGNATURE;
        case EntityKind::TRANSFORMATION: return TRANSFORMATION_ADDED_SIGNATURE;
        case EntityKind::CONDITION: return CONDITION_ADDED_SIGNATURE;
        }
        return TRANSFORMATION_ADDED_SIGNATURE;
    }

    std::string eventTopic(const std::string_view signature)
    {
        return chain::bytes32ToHex(chain::constructEventTopic(std::string(signature)));
    }
}
