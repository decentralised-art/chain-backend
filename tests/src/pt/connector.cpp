#include "unit-tests.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::parse;
using namespace dcn::tests;

namespace
{
    Connector makeConnectorSample()
    {
        Connector connector;
        connector.set_name("connector_beta");

        auto * dim0 = connector.add_dimensions();
        dim0->set_composite("comp_a");
        (*dim0->mutable_bindings())["0"] = "comp_slot_a";
        auto * tx0 = dim0->add_transformations();
        tx0->set_name("transform_a");
        tx0->add_args(1);

        auto * dim1 = connector.add_dimensions();
        dim1->set_composite("comp_b");
        (*dim1->mutable_bindings())["1"] = "comp_slot_b";
        auto * tx1 = dim1->add_transformations();
        tx1->set_name("transform_b");
        tx1->add_args(2);
        tx1->add_args(3);
        connector.set_condition_name("condition_check");
        connector.add_condition_args(1);
        connector.add_condition_args(2);
        return connector;
    }

    ConnectorRecord makeConnectorRecordSample()
    {
        ConnectorRecord record;
        *record.mutable_connector() = makeConnectorSample();
        record.set_owner("0xabc123");
        return record;
    }

    void expectEqual(const TransformationDef & lhs, const TransformationDef & rhs)
    {
        ASSERT_EQ(lhs.name(), rhs.name());
        ASSERT_EQ(lhs.args_size(), rhs.args_size());
        for(int i = 0; i < lhs.args_size(); ++i)
        {
            EXPECT_EQ(lhs.args(i), rhs.args(i));
        }
    }

    void expectEqual(const Dimension & lhs, const Dimension & rhs)
    {
        EXPECT_EQ(lhs.composite(), rhs.composite());
        ASSERT_EQ(lhs.bindings_size(), rhs.bindings_size());
        for(const auto & [slot, composite] : lhs.bindings())
        {
            const auto it = rhs.bindings().find(slot);
            ASSERT_NE(it, rhs.bindings().end());
            EXPECT_EQ(it->second, composite);
        }
        ASSERT_EQ(lhs.transformations_size(), rhs.transformations_size());
        for(int i = 0; i < lhs.transformations_size(); ++i)
        {
            expectEqual(lhs.transformations(i), rhs.transformations(i));
        }
    }

    void expectEqual(const Connector & lhs, const Connector & rhs)
    {
        ASSERT_EQ(lhs.name(), rhs.name());
        ASSERT_EQ(lhs.dimensions_size(), rhs.dimensions_size());
        for(int i = 0; i < lhs.dimensions_size(); ++i)
        {
            expectEqual(lhs.dimensions(i), rhs.dimensions(i));
        }

        ASSERT_EQ(lhs.condition_name(), rhs.condition_name());
        ASSERT_EQ(lhs.condition_args_size(), rhs.condition_args_size());
        for(int i = 0; i < lhs.condition_args_size(); ++i)
        {
            EXPECT_EQ(lhs.condition_args(i), rhs.condition_args(i));
        }
    }

    void expectEqual(const ConnectorRecord & lhs, const ConnectorRecord & rhs)
    {
        expectEqual(lhs.connector(), rhs.connector());
        EXPECT_EQ(lhs.owner(), rhs.owner());
    }
}

TEST_F(UnitTest, Connector_ParseFromJson_JsonAndProtobufMatch)
{
    json json_input = {
        {"name", "connector_beta"},
        {"dimensions", json::array({
            json{
                {"composite", "comp_a"},
                {"bindings", json{{"0", "comp_slot_a"}}},
                {"transformations", json::array({
                    json{{"name", "transform_a"}, {"args", json::array({1})}}
                })}
            },
            json{
                {"composite", "comp_b"},
                {"bindings", json{{"1", "comp_slot_b"}}},
                {"transformations", json::array({
                    json{{"name", "transform_b"}, {"args", json::array({2, 3})}}
                })}
            }
        })},
        {"condition_name", "condition_check"},
        {"condition_args", json::array({1, 2})}
    };

    auto json_connector = parseFromJson<Connector>(json_input, use_json);
    auto protobuf_connector = parseFromJson<Connector>(json_input.dump(), use_protobuf);

    ASSERT_TRUE(json_connector.has_value());
    ASSERT_TRUE(protobuf_connector.has_value());
    expectEqual(*json_connector, *protobuf_connector);
}

TEST_F(UnitTest, Connector_ParseToJson_RoundTripAcrossParsers)
{
    Connector connector = makeConnectorSample();

    auto json_out = parseToJson(connector, use_json);
    auto protobuf_out = parseToJson(connector, use_protobuf);

    ASSERT_TRUE(json_out.has_value());
    ASSERT_TRUE(protobuf_out.has_value());

    auto from_json_via_protobuf = parseFromJson<Connector>(json_out->dump(), use_protobuf);
    auto from_protobuf_via_json = parseFromJson<Connector>(json::parse(*protobuf_out), use_json);

    ASSERT_TRUE(from_json_via_protobuf.has_value());
    ASSERT_TRUE(from_protobuf_via_json.has_value());
    expectEqual(connector, *from_json_via_protobuf);
    expectEqual(connector, *from_protobuf_via_json);
}

TEST_F(UnitTest, Connector_ParseFromJson_RejectsLegacySlotBindings)
{
    json json_input = {
        {"name", "connector_beta"},
        {"dimensions", json::array({
            json{
                {"composite", "comp_a"},
                {"slot_bindings", json{{"0", "TIME"}}},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()}
    };

    auto connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_FALSE(connector.has_value());
}

TEST_F(UnitTest, Connector_ParseFromJson_RejectsNonIntegerTransformationArgs)
{
    json json_input = {
        {"name", "connector_beta"},
        {"dimensions", json::array({
            json{
                {"composite", ""},
                {"transformations", json::array({
                    json{{"name", "transform_a"}, {"args", json::array({"bad"})}}
                })}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()}
    };

    auto connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_FALSE(connector.has_value());
}

TEST_F(UnitTest, Connector_ParseFromJson_RejectsOutOfRangeConditionArgs)
{
    json json_input = {
        {"name", "connector_beta"},
        {"dimensions", json::array({
            json{
                {"composite", ""},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", "condition_check"},
        {"condition_args", json::array({2147483648})}
    };

    auto connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_FALSE(connector.has_value());
}

TEST_F(UnitTest, Connector_ParseFromJson_RejectsScalarDimensionBindings)
{
    json json_input = {
        {"name", "connector_beta"},
        {"dimensions", json::array({
            json{
                {"composite", ""},
                {"bindings", json{{"0", "comp_slot_a"}}},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()}
    };

    auto connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_FALSE(connector.has_value());
    EXPECT_EQ(connector.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ParseFromJson_RejectsNonCanonicalBindingSlotKey)
{
    json json_input = {
        {"name", "connector_beta"},
        {"dimensions", json::array({
            json{
                {"composite", "comp_a"},
                {"bindings", json{{"01", "comp_slot_a"}}},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()}
    };

    auto connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_FALSE(connector.has_value());
    EXPECT_EQ(connector.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ParseFromJson_RejectsConnectorWithoutDimensionsAtParseBoundary)
{
    json json_input = {
        {"name", "connector_without_dimensions"},
        {"dimensions", json::array()},
        {"condition_name", ""},
        {"condition_args", json::array()}
    };

    auto json_connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_FALSE(json_connector.has_value());
    EXPECT_EQ(json_connector.error().kind, ParseError::Kind::INVALID_VALUE);

    auto protobuf_connector = parseFromJson<Connector>(json_input.dump(), use_protobuf);
    ASSERT_FALSE(protobuf_connector.has_value());
    EXPECT_EQ(protobuf_connector.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ParseFromJson_AcceptsStaticRiAtRootAndLastLocalPosition)
{
    json json_input = {
        {"name", "connector_static_ri_boundary_ok"},
        {"dimensions", json::array({
            json{
                {"composite", ""},
                {"transformations", json::array()}
            },
            json{
                {"composite", ""},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()},
        {"static_ri", json::object({
            {"0", json{{"start_point", 3}, {"transformation_shift", 4}}},
            {"2", json{{"start_point", 7}, {"transformation_shift", 8}}}
        })}
    };

    auto json_connector = parseFromJson<Connector>(json_input, use_json);
    auto protobuf_connector = parseFromJson<Connector>(json_input.dump(), use_protobuf);

    ASSERT_TRUE(json_connector.has_value());
    ASSERT_TRUE(protobuf_connector.has_value());
    ASSERT_EQ(json_connector->static_ri_size(), 2);
    ASSERT_EQ(protobuf_connector->static_ri_size(), 2);

    const auto json_root = json_connector->static_ri().find(0);
    const auto json_last = json_connector->static_ri().find(2);
    ASSERT_NE(json_root, json_connector->static_ri().end());
    ASSERT_NE(json_last, json_connector->static_ri().end());
    EXPECT_EQ(json_root->second.start_point(), 3);
    EXPECT_EQ(json_root->second.transformation_shift(), 4);
    EXPECT_EQ(json_last->second.start_point(), 7);
    EXPECT_EQ(json_last->second.transformation_shift(), 8);
}

TEST_F(UnitTest, Connector_ParseFromJson_AcceptsPartialStaticRiMap)
{
    json json_input = {
        {"name", "connector_static_ri_partial_ok"},
        {"dimensions", json::array({
            json{
                {"composite", ""},
                {"transformations", json::array()}
            },
            json{
                {"composite", ""},
                {"transformations", json::array()}
            },
            json{
                {"composite", ""},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()},
        {"static_ri", json::object({
            {"2", json{{"start_point", 11}, {"transformation_shift", 12}}}
        })}
    };

    auto json_connector = parseFromJson<Connector>(json_input, use_json);
    auto protobuf_connector = parseFromJson<Connector>(json_input.dump(), use_protobuf);

    ASSERT_TRUE(json_connector.has_value());
    ASSERT_TRUE(protobuf_connector.has_value());
    ASSERT_EQ(json_connector->static_ri_size(), 1);
    ASSERT_EQ(protobuf_connector->static_ri_size(), 1);

    const auto json_entry = json_connector->static_ri().find(2);
    const auto protobuf_entry = protobuf_connector->static_ri().find(2);
    ASSERT_NE(json_entry, json_connector->static_ri().end());
    ASSERT_NE(protobuf_entry, protobuf_connector->static_ri().end());
    EXPECT_EQ(json_entry->second.start_point(), 11);
    EXPECT_EQ(json_entry->second.transformation_shift(), 12);
    EXPECT_EQ(protobuf_entry->second.start_point(), 11);
    EXPECT_EQ(protobuf_entry->second.transformation_shift(), 12);
}

TEST_F(UnitTest, Connector_ParseFromJson_AcceptsStaticRiKeyBeyondLocalDimensionCount)
{
    json json_input = {
        {"name", "connector_static_ri_oob"},
        {"dimensions", json::array({
            json{
                {"composite", ""},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()},
        {"static_ri", json::object({
            {"2", json{{"start_point", 1}, {"transformation_shift", 2}}}
        })}
    };

    auto json_connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_TRUE(json_connector.has_value());
    ASSERT_EQ(json_connector->static_ri_size(), 1);
    const auto json_entry = json_connector->static_ri().find(2);
    ASSERT_NE(json_entry, json_connector->static_ri().end());
    EXPECT_EQ(json_entry->second.start_point(), 1);
    EXPECT_EQ(json_entry->second.transformation_shift(), 2);

    auto protobuf_connector = parseFromJson<Connector>(json_input.dump(), use_protobuf);
    ASSERT_TRUE(protobuf_connector.has_value());
    ASSERT_EQ(protobuf_connector->static_ri_size(), 1);
    const auto protobuf_entry = protobuf_connector->static_ri().find(2);
    ASSERT_NE(protobuf_entry, protobuf_connector->static_ri().end());
    EXPECT_EQ(protobuf_entry->second.start_point(), 1);
    EXPECT_EQ(protobuf_entry->second.transformation_shift(), 2);
}

TEST_F(UnitTest, Connector_ParseFromJson_RejectsNonCanonicalStaticRiKey)
{
    json json_input = {
        {"name", "connector_static_ri_bad_key"},
        {"dimensions", json::array({
            json{
                {"composite", ""},
                {"transformations", json::array()}
            }
        })},
        {"condition_name", ""},
        {"condition_args", json::array()},
        {"static_ri", json::object({
            {"01", json{{"start_point", 1}, {"transformation_shift", 2}}}
        })}
    };

    auto connector = parseFromJson<Connector>(json_input, use_json);
    ASSERT_FALSE(connector.has_value());
    EXPECT_EQ(connector.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ParseFromJson_DuplicateStaticRiKeyInRawJson_UsesSingleMapEntry)
{
    const std::string raw_json_input = R"JSON(
{
  "name": "connector_static_ri_duplicate_key",
  "dimensions": [
    {
      "composite": "",
      "transformations": []
    }
  ],
  "condition_name": "",
  "condition_args": [],
  "static_ri": {
    "0": {"start_point": 1, "transformation_shift": 2},
    "0": {"start_point": 9, "transformation_shift": 10}
  }
}
)JSON";

    auto json_connector = parseFromJson<Connector>(json::parse(raw_json_input), use_json);
    auto protobuf_connector = parseFromJson<Connector>(raw_json_input, use_protobuf);

    ASSERT_TRUE(json_connector.has_value());
    ASSERT_FALSE(protobuf_connector.has_value());
    EXPECT_EQ(protobuf_connector.error().kind, ParseError::Kind::INVALID_VALUE);
    ASSERT_EQ(json_connector->static_ri_size(), 1);

    const auto json_it = json_connector->static_ri().find(0);
    ASSERT_NE(json_it, json_connector->static_ri().end());

    EXPECT_EQ(json_it->second.start_point(), 9);
    EXPECT_EQ(json_it->second.transformation_shift(), 10);
}

TEST_F(UnitTest, ConnectorRecord_ParseFromJson_JsonAndProtobufMatch)
{
    json json_connector = {
        {"name", "connector_beta"},
        {"dimensions", json::array({
            json{
                {"composite", "comp_a"},
                {"bindings", json{{"0", "comp_slot_a"}}},
                {"transformations", json::array({
                    json{{"name", "transform_a"}, {"args", json::array({1})}}
                })}
            },
            json{
                {"composite", "comp_b"},
                {"bindings", json{{"1", "comp_slot_b"}}},
                {"transformations", json::array({
                    json{{"name", "transform_b"}, {"args", json::array({2, 3})}}
                })}
            }
        })},
        {"condition_name", "condition_check"},
        {"condition_args", json::array({1, 2})}
    };
    json json_input = {
        {"connector", json_connector},
        {"owner", "0xabc123"}
    };

    auto json_record = parseFromJson<ConnectorRecord>(json_input, use_json);
    auto protobuf_record = parseFromJson<ConnectorRecord>(json_input.dump(), use_protobuf);

    ASSERT_TRUE(json_record.has_value());
    ASSERT_TRUE(protobuf_record.has_value());
    expectEqual(*json_record, *protobuf_record);
}

TEST_F(UnitTest, ConnectorRecord_ParseToJson_RoundTripAcrossParsers)
{
    ConnectorRecord record = makeConnectorRecordSample();

    auto json_out = parseToJson(record, use_json);
    auto protobuf_out = parseToJson(record, use_protobuf);

    ASSERT_TRUE(json_out.has_value());
    ASSERT_TRUE(protobuf_out.has_value());

    auto from_json_via_protobuf = parseFromJson<ConnectorRecord>(json_out->dump(), use_protobuf);
    auto from_protobuf_via_json = parseFromJson<ConnectorRecord>(json::parse(*protobuf_out), use_json);

    ASSERT_TRUE(from_json_via_protobuf.has_value());
    ASSERT_TRUE(from_protobuf_via_json.has_value());
    expectEqual(record, *from_json_via_protobuf);
    expectEqual(record, *from_protobuf_via_json);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_UsesConstructorPattern)
{
    Connector connector = makeConnectorSample();
    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_TRUE(solidity_result.has_value());
    const std::string & solidity = *solidity_result;

    EXPECT_NE(solidity.find("constructor(address registryAddr, bytes32 contentHash_, bytes32 metadataHash_) "), std::string::npos);
    EXPECT_NE(solidity.find("__ConnectorBase_finalizeInit"), std::string::npos);
    EXPECT_NE(solidity.find("function _compositeDimIds()"), std::string::npos);
    EXPECT_NE(solidity.find("function _compositeNames()"), std::string::npos);
    EXPECT_NE(solidity.find("function _bindingDimIds()"), std::string::npos);
    EXPECT_NE(solidity.find("function _bindingSlotIds()"), std::string::npos);
    EXPECT_NE(solidity.find("function _bindingNames()"), std::string::npos);
    EXPECT_EQ(solidity.find("function initialize(address registryAddr) external initializer"), std::string::npos);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsNonNumericBindingSlots)
{
    Connector connector = makeConnectorSample();
    connector.mutable_dimensions(0)->clear_bindings();
    (*connector.mutable_dimensions(0)->mutable_bindings())["dim:0"] = "comp_slot_a";

    auto solidity_result = constructConnectorSolidityCode(connector);
    EXPECT_FALSE(solidity_result.has_value());
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsConnectorWithoutDimensions)
{
    Connector connector;
    connector.set_name("connector_without_dimensions");

    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsInvalidContractIdentifier)
{
    Connector connector = makeConnectorSample();
    connector.set_name("bad.connector");

    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsReservedKeywordContractIdentifier)
{
    Connector connector = makeConnectorSample();
    connector.set_name("mapping");

    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsTransformationWithEmptyName)
{
    Connector connector = makeConnectorSample();
    connector.mutable_dimensions(0)->mutable_transformations(0)->set_name("");

    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsCanonicalDuplicateBindingSlots)
{
    Connector connector = makeConnectorSample();
    connector.mutable_dimensions(0)->clear_bindings();
    (*connector.mutable_dimensions(0)->mutable_bindings())["1"] = "comp_slot_a";
    (*connector.mutable_dimensions(0)->mutable_bindings())["01"] = "comp_slot_b";

    auto solidity_result = constructConnectorSolidityCode(connector);
    EXPECT_FALSE(solidity_result.has_value());
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsInvalidCompositeConnectorIdentifier)
{
    Connector connector = makeConnectorSample();
    connector.mutable_dimensions(0)->set_composite("bad-composite");

    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsInvalidBindingTargetIdentifier)
{
    Connector connector = makeConnectorSample();
    connector.mutable_dimensions(0)->clear_bindings();
    (*connector.mutable_dimensions(0)->mutable_bindings())["0"] = "bad\"target\\name\nline";

    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Connector_ConstructSolidityCode_RejectsReservedKeywordBindingTargetIdentifier)
{
    Connector connector = makeConnectorSample();
    connector.mutable_dimensions(0)->clear_bindings();
    (*connector.mutable_dimensions(0)->mutable_bindings())["0"] = "mapping";

    auto solidity_result = constructConnectorSolidityCode(connector);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

namespace
{
    // ABI-encoding helpers for the ConnectorAdded event used to exercise
    // decodeConnectorAddedEvent / buildConnectorRecordFromEvent without an EVM deploy.
    // Layout MUST match the Solidity emit order and dcn::chain::decodeAbi* readers.
    void appendUintWord(std::vector<std::uint8_t> & out, std::uint64_t value)
    {
        std::array<std::uint8_t, 32> word{};
        for(std::size_t i = 0; i < sizeof(std::uint64_t); ++i)
        {
            word[31 - i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
        }
        out.insert(out.end(), word.begin(), word.end());
    }

    void appendUint32Word(std::vector<std::uint8_t> & out, std::uint32_t value)
    {
        appendUintWord(out, static_cast<std::uint64_t>(value));
    }

    void appendInt32Word(std::vector<std::uint8_t> & out, std::int32_t value)
    {
        const std::uint32_t bits = static_cast<std::uint32_t>(value);
        const std::uint8_t fill = (value < 0) ? 0xFFu : 0x00u;
        std::array<std::uint8_t, 32> word{};
        word.fill(fill);
        word[28] = static_cast<std::uint8_t>((bits >> 24) & 0xFFu);
        word[29] = static_cast<std::uint8_t>((bits >> 16) & 0xFFu);
        word[30] = static_cast<std::uint8_t>((bits >> 8) & 0xFFu);
        word[31] = static_cast<std::uint8_t>(bits & 0xFFu);
        out.insert(out.end(), word.begin(), word.end());
    }

    void appendBytes32Word(std::vector<std::uint8_t> & out, const evmc::bytes32 & value)
    {
        out.insert(out.end(), std::begin(value.bytes), std::end(value.bytes));
    }

    void appendAddressWord(std::vector<std::uint8_t> & out, const chain::Address & address)
    {
        std::array<std::uint8_t, 32> word{};
        std::memcpy(word.data() + 12, address.bytes, sizeof(address.bytes));
        out.insert(out.end(), word.begin(), word.end());
    }

    std::vector<std::uint8_t> encodeUint32Array(const std::vector<std::uint32_t> & values)
    {
        std::vector<std::uint8_t> out;
        appendUintWord(out, values.size());
        for(const std::uint32_t value : values)
        {
            appendUint32Word(out, value);
        }
        return out;
    }

    std::vector<std::uint8_t> encodeInt32Array(const std::vector<std::int32_t> & values)
    {
        std::vector<std::uint8_t> out;
        appendUintWord(out, values.size());
        for(const std::int32_t value : values)
        {
            appendInt32Word(out, value);
        }
        return out;
    }

    std::vector<std::uint8_t> encodeString(const std::string & value)
    {
        std::vector<std::uint8_t> out;
        appendUintWord(out, value.size());
        out.insert(
            out.end(),
            reinterpret_cast<const std::uint8_t *>(value.data()),
            reinterpret_cast<const std::uint8_t *>(value.data()) + value.size());
        const std::size_t padding = (32 - (value.size() % 32)) % 32;
        out.insert(out.end(), padding, 0);
        return out;
    }

    std::vector<std::uint8_t> encodeStringArray(const std::vector<std::string> & values)
    {
        std::vector<std::uint8_t> out;
        appendUintWord(out, values.size());

        std::vector<std::vector<std::uint8_t>> encoded;
        encoded.reserve(values.size());
        for(const std::string & value : values)
        {
            encoded.push_back(encodeString(value));
        }

        // Offsets are relative to the start of the area right after the length word.
        std::uint64_t offset = static_cast<std::uint64_t>(values.size()) * 32u;
        for(const auto & blob : encoded)
        {
            appendUintWord(out, offset);
            offset += blob.size();
        }
        for(const auto & blob : encoded)
        {
            out.insert(out.end(), blob.begin(), blob.end());
        }
        return out;
    }

    std::string toHexString(const std::uint8_t * data, std::size_t size)
    {
        static const char * digits = "0123456789abcdef";
        std::string out;
        out.reserve(size * 2);
        for(std::size_t i = 0; i < size; ++i)
        {
            out.push_back(digits[(data[i] >> 4) & 0x0F]);
            out.push_back(digits[data[i] & 0x0F]);
        }
        return out;
    }

    ConnectorRecord makeConnectorWithTransformations()
    {
        ConnectorRecord record;
        // Canonical owner string so a round-tripped addressToHex compares equal.
        chain::Address owner{};
        owner.bytes[19] = 0xAB;
        owner.bytes[18] = 0xCD;
        record.set_owner(chain::addressToHex(owner));

        Connector * connector = record.mutable_connector();
        connector->set_name("connector_with_tf");

        // dim0: transformation "scale" with args [2, 3].
        Dimension * dim0 = connector->add_dimensions();
        dim0->set_composite("");
        TransformationDef * scale = dim0->add_transformations();
        scale->set_name("scale");
        scale->add_args(2);
        scale->add_args(3);

        // dim1: transformation "shift" with no args.
        Dimension * dim1 = connector->add_dimensions();
        dim1->set_composite("");
        TransformationDef * shift = dim1->add_transformations();
        shift->set_name("shift");

        return record;
    }
}

namespace dcn::tests
{
    struct EncodedConnectorAddedEvent
    {
        std::string data_hex;
        std::vector<std::string> topics_hex;
    };

    EncodedConnectorAddedEvent encodeConnectorAddedForTest(const ConnectorRecord & record)
    {
        const Connector & connector = record.connector();

        // Build the parallel transformation arrays in (dimId, indexWithinDim) order,
        // mirroring the Solidity codegen in src/pt/src/connector.cpp.
        std::vector<std::uint32_t> transformation_dim_ids;
        std::vector<std::string> transformation_names;
        std::vector<std::uint32_t> transformation_arg_counts;
        std::vector<std::int32_t> transformation_args;
        for(int i = 0; i < connector.dimensions_size(); ++i)
        {
            for(int j = 0; j < connector.dimensions(i).transformations_size(); ++j)
            {
                const TransformationDef & tf = connector.dimensions(i).transformations(j);
                transformation_dim_ids.push_back(static_cast<std::uint32_t>(i));
                transformation_names.push_back(tf.name());
                transformation_arg_counts.push_back(static_cast<std::uint32_t>(tf.args_size()));
                for(int k = 0; k < tf.args_size(); ++k)
                {
                    transformation_args.push_back(tf.args(k));
                }
            }
        }

        std::vector<std::int32_t> condition_args;
        for(int i = 0; i < connector.condition_args_size(); ++i)
        {
            condition_args.push_back(connector.condition_args(i));
        }

        constexpr std::uint64_t head_words = 21;
        const std::uint64_t head_bytes = head_words * 32u;

        std::vector<std::uint8_t> head;
        std::vector<std::uint8_t> tails;
        head.reserve(static_cast<std::size_t>(head_bytes));

        const auto addDynamic = [&](std::vector<std::uint8_t> blob)
        {
            appendUintWord(head, head_bytes + tails.size());
            tails.insert(tails.end(), blob.begin(), blob.end());
        };

        chain::Address connector_address{};
        evmc::bytes32 format_hash{};

        // 0 name (string)
        addDynamic(encodeString(connector.name()));
        // 1 connectorAddr (address)
        appendAddressWord(head, connector_address);
        // 2 dimensionsCount (uint32)
        appendUint32Word(head, static_cast<std::uint32_t>(connector.dimensions_size()));
        // 3 compositeDimIds (uint32[])
        addDynamic(encodeUint32Array({}));
        // 4 compositeNames (string[])
        addDynamic(encodeStringArray({}));
        // 5 bindingDimIds (uint32[])
        addDynamic(encodeUint32Array({}));
        // 6 bindingSlotIds (uint32[])
        addDynamic(encodeUint32Array({}));
        // 7 bindingNames (string[])
        addDynamic(encodeStringArray({}));
        // 8 conditionName (string)
        addDynamic(encodeString(connector.condition_name()));
        // 9 conditionArgs (int32[])
        addDynamic(encodeInt32Array(condition_args));
        // 10 formatHash (bytes32)
        appendBytes32Word(head, format_hash);
        // 11 staticRiPositions (uint32[])
        addDynamic(encodeUint32Array({}));
        // 12 staticRiStartPoints (uint32[])
        addDynamic(encodeUint32Array({}));
        // 13 staticRiTransformShifts (uint32[])
        addDynamic(encodeUint32Array({}));
        // 14 transformationDimIds (uint32[])
        addDynamic(encodeUint32Array(transformation_dim_ids));
        // 15 transformationNames (string[])
        addDynamic(encodeStringArray(transformation_names));
        // 16 transformationArgCounts (uint32[])
        addDynamic(encodeUint32Array(transformation_arg_counts));
        // 17 transformationArgs (int32[])
        addDynamic(encodeInt32Array(transformation_args));
        // 18 contentHash (bytes32)
        appendBytes32Word(head, evmc::bytes32{});
        // 19 metadataHash (bytes32)
        appendBytes32Word(head, evmc::bytes32{});
        // 20 runtimeCodeHash (bytes32)
        appendBytes32Word(head, evmc::bytes32{});

        std::vector<std::uint8_t> data = std::move(head);
        data.insert(data.end(), tails.begin(), tails.end());

        EncodedConnectorAddedEvent out;
        out.data_hex = toHexString(data.data(), data.size());

        const evmc::bytes32 topic0 = chain::constructEventTopic(
            "ConnectorAdded(address,address,string,address,uint32,uint32[],string[],uint32[],uint32[],string[],string,int32[],bytes32,uint32[],uint32[],uint32[],uint32[],string[],uint32[],int32[],bytes32,bytes32,bytes32)");
        out.topics_hex.push_back(toHexString(topic0.bytes, sizeof(topic0.bytes)));

        // topic[1] = caller (indexed), topic[2] = owner (indexed).
        const auto owner_address = evmc::from_hex<chain::Address>(record.owner());
        chain::Address owner = owner_address.value_or(chain::Address{});
        std::array<std::uint8_t, 32> caller_word{};
        out.topics_hex.push_back(toHexString(caller_word.data(), caller_word.size()));
        std::array<std::uint8_t, 32> owner_word{};
        std::memcpy(owner_word.data() + 12, owner.bytes, sizeof(owner.bytes));
        out.topics_hex.push_back(toHexString(owner_word.data(), owner_word.size()));

        return out;
    }
}

TEST(PtConnectorEvent, DecodeRecoversTransformationDefs)
{
    const ConnectorRecord record = makeConnectorWithTransformations();
    const auto event_bytes = dcn::tests::encodeConnectorAddedForTest(record);
    const auto decoded = dcn::pt::decodeConnectorAddedEvent(event_bytes.data_hex, event_bytes.topics_hex);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->transformations.at(0).size(), 1u);
    EXPECT_EQ(decoded->transformations.at(0)[0].name(), "scale");
    ASSERT_EQ(decoded->transformations.at(0)[0].args_size(), 2);
    EXPECT_EQ(decoded->transformations.at(0)[0].args(0), 2);
    EXPECT_EQ(decoded->transformations.at(1)[0].name(), "shift");
    EXPECT_EQ(decoded->transformations.at(1)[0].args_size(), 0);
}

TEST(PtConnectorEvent, BuildRecordRoundTrips)
{
    const ConnectorRecord original = makeConnectorWithTransformations();
    const auto enc = dcn::tests::encodeConnectorAddedForTest(original);
    const auto decoded = dcn::pt::decodeConnectorAddedEvent(enc.data_hex, enc.topics_hex);
    ASSERT_TRUE(decoded.has_value());

    const ConnectorRecord rebuilt = dcn::pt::buildConnectorRecordFromEvent(*decoded);

    EXPECT_EQ(rebuilt.connector().name(), original.connector().name());
    EXPECT_EQ(rebuilt.connector().dimensions_size(), original.connector().dimensions_size());
    EXPECT_EQ(rebuilt.owner(), original.owner());
}
