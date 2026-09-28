#include "unit-tests.hpp"

#include "events_test_harness.hpp"
#include "registry_events.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

// Forward declarations for connector-event ABI encoding helpers defined in
// tests/src/pt/connector.cpp (compiled into the same binary).
namespace dcn::tests
{
    struct EncodedConnectorAddedEvent
    {
        std::string data_hex;
        std::vector<std::string> topics_hex;
    };
    EncodedConnectorAddedEvent encodeConnectorAddedForTest(const ConnectorRecord & record);
}

TEST_F(UnitTest, Events_Decoder_ConnectorAddedEvent_DecodesTransformationDefs)
{
    events::PTEventDecoder decoder;

    // Build a ConnectorRecord with two scalar dimensions, each with one transformation def.
    ConnectorRecord record;
    chain::Address owner{};
    owner.bytes[19] = 0xAB;
    record.set_owner(chain::addressToHex(owner));

    Connector * connector = record.mutable_connector();
    connector->set_name("decode_tf_test");

    // dim0: transformation "scale" with args [2, 3]
    Dimension * dim0 = connector->add_dimensions();
    dim0->set_composite("");
    TransformationDef * scale = dim0->add_transformations();
    scale->set_name("scale");
    scale->add_args(2);
    scale->add_args(3);

    // dim1: transformation "shift" with no args
    Dimension * dim1 = connector->add_dimensions();
    dim1->set_composite("");
    TransformationDef * shift = dim1->add_transformations();
    shift->set_name("shift");

    // Encode via the connector-event ABI helper (defined in tests/src/pt/connector.cpp).
    const auto encoded = dcn::tests::encodeConnectorAddedForTest(record);

    // makeRawLog sets only topics[0]; patch topics[1] (caller) and topics[2] (owner).
    events::RawChainLog log = makeRawLog(
        100,
        2,
        5,
        hexBytes(0xC1, 32),
        hexBytes(0xD1, 32),
        encoded.topics_hex[0],
        encoded.data_hex,
        false,
        1'700'000'000,
        1'700'000'002'000);
    log.topics[1] = encoded.topics_hex[1];
    log.topics[2] = encoded.topics_hex[2];

    const auto decoded = decoder.decode(log);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event_type, events::EventType::CONNECTOR_ADDED);

    const auto payload = json::parse(decoded->decoded_json, nullptr, false);
    ASSERT_FALSE(payload.is_discarded());
    ASSERT_TRUE(payload.contains("transformations"));
    ASSERT_TRUE(payload.at("transformations").is_array());

    const auto & tfs = payload.at("transformations");
    ASSERT_EQ(tfs.size(), 2u);

    // dim0 -> "scale" with args [2, 3]
    EXPECT_EQ(tfs[0].value("dim_id", std::uint32_t{9999}), 0u);
    EXPECT_EQ(tfs[0].value("name", std::string{}), std::string{"scale"});
    ASSERT_EQ(tfs[0].at("args").size(), 2u);
    EXPECT_EQ(tfs[0].at("args")[0].get<int>(), 2);
    EXPECT_EQ(tfs[0].at("args")[1].get<int>(), 3);

    // dim1 -> "shift" with no args
    EXPECT_EQ(tfs[1].value("dim_id", std::uint32_t{9999}), 1u);
    EXPECT_EQ(tfs[1].value("name", std::string{}), std::string{"shift"});
    EXPECT_EQ(tfs[1].at("args").size(), 0u);
}

TEST_F(UnitTest, Events_Decoder_TransformationEvent_DecodesCanonicalFields)
{
    events::PTEventDecoder decoder;

    const chain::Address caller = makeAddressFromByte(0x11);
    const chain::Address owner = makeAddressFromByte(0x22);
    const chain::Address entity = makeAddressFromByte(0x33);
    const std::string topic0 = topicForEvent("TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)");
    const std::string data = encodeSimpleAddedEventDataV2(caller, "TransformAlpha", entity, owner, 3);

    events::RawChainLog log = makeRawLog(
        100,
        2,
        5,
        hexBytes(0xA1, 32),
        hexBytes(0xB1, 32),
        topic0,
        data,
        false,
        1'700'000'000,
        1'700'000'001'000);

    const auto decoded = decoder.decode(log);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event_type, events::EventType::TRANSFORMATION_ADDED);
    EXPECT_EQ(decoded->state, events::EventState::OBSERVED);
    EXPECT_EQ(decoded->name, "TransformAlpha");
    EXPECT_EQ(decoded->caller, chain::normalizeHex(evmc::hex(caller)));
    EXPECT_EQ(decoded->owner, chain::normalizeHex(evmc::hex(owner)));
    EXPECT_EQ(decoded->entity_address, chain::normalizeHex(evmc::hex(entity)));
    ASSERT_TRUE(decoded->args_count.has_value());
    EXPECT_EQ(*decoded->args_count, 3u);

    const auto payload = json::parse(decoded->decoded_json, nullptr, false);
    ASSERT_FALSE(payload.is_discarded());
    EXPECT_EQ(payload.value("name", ""), "TransformAlpha");
    EXPECT_EQ(payload.value("caller", ""), chain::normalizeHex(evmc::hex(caller)));
    EXPECT_EQ(payload.value("owner", ""), chain::normalizeHex(evmc::hex(owner)));
    EXPECT_EQ(payload.value("transformation_address", ""), chain::normalizeHex(evmc::hex(entity)));
    EXPECT_EQ(payload.value("args_count", 0), 3);
}

TEST_F(UnitTest, Events_Decoder_RejectsNamesThisServerCannotRepresent)
{
    events::PTEventDecoder decoder;
    const std::string topic0 = topicForEvent("TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)");

    // A name is a Solidity identifier and a file name here; anything else a chain
    // event carries, including bytes that are not UTF-8, is refused without throwing.
    for(const std::string & name : {std::string("has space"), std::string("../escape"), std::string("ÿþ"), std::string(129, 'a')})
    {
        const std::string data = encodeSimpleAddedEventDataV2(
            makeAddressFromByte(0x11), name, makeAddressFromByte(0x33), makeAddressFromByte(0x22), 1);
        const events::RawChainLog log = makeRawLog(
            100, 0, 0, hexBytes(0xA1, 32), hexBytes(0xB1, 32), topic0, data, false, 1'700'000'000, 1'000);

        std::optional<events::DecodedEvent> decoded;
        EXPECT_NO_THROW(decoded = decoder.decode(log));
        EXPECT_FALSE(decoded.has_value());
    }
}

TEST_F(UnitTest, Events_Decoder_UnsupportedTopic_ReturnsNullopt)
{
    events::PTEventDecoder decoder;

    events::RawChainLog log = makeRawLog(
        100,
        0,
        0,
        hexBytes(0xA2, 32),
        hexBytes(0xB2, 32),
        topicForEvent("CompletelyUnknown(address,uint256)"),
        "0x00",
        false,
        1'700'000'010,
        1'700'000'011'000);

    const auto decoded = decoder.decode(log);
    EXPECT_FALSE(decoded.has_value());
}

TEST_F(UnitTest, Events_Decoder_MalformedPayload_ReturnsNullopt)
{
    events::PTEventDecoder decoder;

    events::RawChainLog log = makeRawLog(
        100,
        0,
        0,
        hexBytes(0xA3, 32),
        hexBytes(0xB3, 32),
        topicForEvent("ConditionAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)"),
        "0x1234",
        false,
        1'700'000'020,
        1'700'000'021'000);

    const auto decoded = decoder.decode(log);
    EXPECT_FALSE(decoded.has_value());
}

TEST_F(UnitTest, Events_Decoder_RemovedFlag_MapsToRemovedState)
{
    events::PTEventDecoder decoder;

    const chain::Address caller = makeAddressFromByte(0x15);
    const chain::Address owner = makeAddressFromByte(0x16);
    const chain::Address entity = makeAddressFromByte(0x17);

    events::RawChainLog log = makeRawLog(
        101,
        1,
        2,
        hexBytes(0xA4, 32),
        hexBytes(0xB4, 32),
        topicForEvent("ConditionAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)"),
        encodeSimpleAddedEventDataV2(caller, "ConditionAlpha", entity, owner, 1),
        true,
        1'700'000'030,
        1'700'000'031'000);

    const auto decoded = decoder.decode(log);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event_type, events::EventType::CONDITION_ADDED);
    EXPECT_EQ(decoded->state, events::EventState::REMOVED);
}

TEST_F(UnitTest, Events_Decoder_MissingTopic0_ReturnsNullopt)
{
    events::PTEventDecoder decoder;

    events::RawChainLog log{};
    log.chain_id = CHAIN_ID;
    log.block_number = 10;
    log.block_hash = hexBytes(0xAA, 32);
    log.tx_hash = hexBytes(0xBB, 32);
    log.log_index = 1;
    log.tx_index = 1;
    log.address = hexAddress(0x11);
    log.data_hex = "0x";
    log.seen_at_ms = 1'700'000'032'000;

    const auto decoded = decoder.decode(log);
    EXPECT_FALSE(decoded.has_value());
}

TEST_F(UnitTest, Events_ParseRawLog_RejectsLogMissingRequiredFields)
{
    json log_json = json::object();
    log_json["blockNumber"] = "0x10";
    // intentionally omit "blockHash" and other required fields
    log_json["topics"] = json::array();

    const auto parsed = parse::parseRawLog(log_json, 1'700'000'050'000, CHAIN_ID);
    EXPECT_FALSE(parsed.has_value());
}

TEST_F(UnitTest, Events_ParseRawLog_RejectsLogWithMalformedHexQuantity)
{
    json log_json = json::object();
    log_json["blockNumber"] = "0xZZZZ";
    log_json["blockHash"] = hexBytes(0xA0, 32);
    log_json["transactionHash"] = hexBytes(0xB0, 32);
    log_json["transactionIndex"] = "0x0";
    log_json["logIndex"] = "0x0";
    log_json["address"] = hexAddress(0x11);
    log_json["data"] = "0x";
    log_json["topics"] = json::array({hexBytes(0xC0, 32)});

    const auto parsed = parse::parseRawLog(log_json, 1'700'000'051'000, CHAIN_ID);
    EXPECT_FALSE(parsed.has_value());
}

TEST_F(UnitTest, Events_ParseRawLog_AcceptsWellFormedLog)
{
    json log_json = json::object();
    log_json["blockNumber"] = "0x10";
    log_json["blockHash"] = hexBytes(0xA0, 32);
    log_json["transactionHash"] = hexBytes(0xB0, 32);
    log_json["transactionIndex"] = "0x1";
    log_json["logIndex"] = "0x2";
    log_json["address"] = hexAddress(0x11);
    log_json["data"] = "0x";
    log_json["topics"] = json::array({hexBytes(0xC0, 32)});

    const auto parsed = parse::parseRawLog(log_json, 1'700'000'052'000, CHAIN_ID);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->block_number, 0x10);
    EXPECT_EQ(parsed->tx_index, 0x1);
    EXPECT_EQ(parsed->log_index, 0x2);
}

TEST_F(UnitTest, Events_Decoder_RemovedEvent_DecodesOwnerEntityAndHashes)
{
    events::PTEventDecoder decoder;

    const chain::Address caller = makeAddressFromByte(0x21);
    const chain::Address owner = makeAddressFromByte(0x22);
    const chain::Address entity = makeAddressFromByte(0x23);

    const events::RawChainLog log = makeRawLog(
        120,
        0,
        0,
        hexBytes(0xA5, 32),
        hexBytes(0xB5, 32),
        pt::eventTopic(pt::TRANSFORMATION_REMOVED_SIGNATURE),
        encodeRemovedEventData(caller, owner, "RemovedAlpha", entity, 0xC1, 0xC2),
        false,
        1'700'000'040,
        1'700'000'041'000);

    const auto decoded = decoder.decode(log);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event_type, events::EventType::TRANSFORMATION_REMOVED);
    EXPECT_EQ(decoded->name, "RemovedAlpha");
    EXPECT_EQ(decoded->caller, chain::addressToHex(caller));
    EXPECT_EQ(decoded->owner, chain::addressToHex(owner));
    EXPECT_EQ(decoded->entity_address, chain::addressToHex(entity));
    EXPECT_EQ(decoded->content_hash.value_or(""), hexBytes(0xC1, 32));
    EXPECT_EQ(decoded->metadata_hash.value_or(""), hexBytes(0xC2, 32));
    ASSERT_TRUE(decoded->entity_kind.has_value());
    EXPECT_EQ(*decoded->entity_kind, pt::EntityKind::TRANSFORMATION);
}

TEST_F(UnitTest, Events_Decoder_EntityOwnerChanged_DecodesKindAndOwners)
{
    events::PTEventDecoder decoder;

    const chain::Address entity = makeAddressFromByte(0x31);
    const chain::Address old_owner = makeAddressFromByte(0x32);
    const chain::Address new_owner = makeAddressFromByte(0x33);

    const events::RawChainLog log = makeRawLog(
        121,
        0,
        0,
        hexBytes(0xA6, 32),
        hexBytes(0xB6, 32),
        pt::eventTopic(pt::ENTITY_OWNER_CHANGED_SIGNATURE),
        encodeOwnerChangedEventData(2, "OwnedCondition", entity, old_owner, new_owner),
        false,
        1'700'000'050,
        1'700'000'051'000);

    const auto decoded = decoder.decode(log);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event_type, events::EventType::ENTITY_OWNER_CHANGED);
    EXPECT_EQ(decoded->name, "OwnedCondition");
    ASSERT_TRUE(decoded->entity_kind.has_value());
    EXPECT_EQ(*decoded->entity_kind, pt::EntityKind::CONDITION);
    EXPECT_EQ(decoded->entity_address, chain::addressToHex(entity));
    EXPECT_EQ(decoded->old_owner.value_or(""), chain::addressToHex(old_owner));
    EXPECT_EQ(decoded->owner, chain::addressToHex(new_owner));

    const auto payload = json::parse(decoded->decoded_json, nullptr, false);
    ASSERT_FALSE(payload.is_discarded());
    EXPECT_EQ(payload.value("kind", ""), "condition");
    EXPECT_EQ(payload.value("new_owner", ""), chain::addressToHex(new_owner));
}

TEST_F(UnitTest, Events_Decoder_EntityOwnerChanged_RejectsUnknownKind)
{
    events::PTEventDecoder decoder;

    const events::RawChainLog log = makeRawLog(
        122,
        0,
        0,
        hexBytes(0xA7, 32),
        hexBytes(0xB7, 32),
        pt::eventTopic(pt::ENTITY_OWNER_CHANGED_SIGNATURE),
        encodeOwnerChangedEventData(3, "Unknown", makeAddressFromByte(0x41), makeAddressFromByte(0x42), makeAddressFromByte(0x43)),
        false,
        1'700'000'060,
        1'700'000'061'000);

    EXPECT_FALSE(decoder.decode(log).has_value());
}
