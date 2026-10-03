#include "unit-tests.hpp"

using namespace dcn;
using namespace dcn::parse;
using namespace dcn::tests;

namespace
{
    Condition makeConditionSample()
    {
        Condition condition;
        condition.set_name("condition_check");
        condition.set_sol_src("return true;");
        return condition;
    }

    ConditionRecord makeConditionRecordSample()
    {
        ConditionRecord record;
        *record.mutable_condition() = makeConditionSample();
        record.set_owner("0xabc123");
        return record;
    }

    // Compares the chain-derivable identity. sol_src is a local-only deploy input that the
    // use_json serializer intentionally drops, so it is not part of this equality.
    void expectEqual(const Condition & lhs, const Condition & rhs)
    {
        ASSERT_EQ(lhs.name(), rhs.name());
        ASSERT_EQ(lhs.args_count(), rhs.args_count());
    }

    void expectEqual(const ConditionRecord & lhs, const ConditionRecord & rhs)
    {
        expectEqual(lhs.condition(), rhs.condition());
        EXPECT_EQ(lhs.owner(), rhs.owner());
    }
}

TEST_F(UnitTest, Condition_ParseFromJson_JsonAndProtobufMatch)
{
    json json_input = {
        {"name", "condition_check"},
        {"sol_src", "return true;"}
    };

    auto json_condition = parseFromJson<Condition>(json_input, use_json);
    auto protobuf_condition = parseFromJson<Condition>(json_input.dump(), use_protobuf);

    ASSERT_TRUE(json_condition.has_value());
    ASSERT_TRUE(protobuf_condition.has_value());
    expectEqual(*json_condition, *protobuf_condition);
}

TEST_F(UnitTest, Condition_ParseToJson_RoundTripAcrossParsers)
{
    Condition condition = makeConditionSample();

    auto json_out = parseToJson(condition, use_json);
    auto protobuf_out = parseToJson(condition, use_protobuf);

    ASSERT_TRUE(json_out.has_value());
    ASSERT_TRUE(protobuf_out.has_value());

    auto from_json_via_protobuf = parseFromJson<Condition>(json_out->dump(), use_protobuf);
    auto from_protobuf_via_json = parseFromJson<Condition>(json::parse(*protobuf_out), use_json);

    ASSERT_TRUE(from_json_via_protobuf.has_value());
    ASSERT_TRUE(from_protobuf_via_json.has_value());
    expectEqual(condition, *from_json_via_protobuf);
    expectEqual(condition, *from_protobuf_via_json);
}

TEST_F(UnitTest, ConditionRecord_ParseFromJson_JsonAndProtobufMatch)
{
    json json_condition = {
        {"name", "condition_check"},
        {"sol_src", "return true;"}
    };
    json json_input = {
        {"condition", json_condition},
        {"owner", "0xabc123"}
    };

    auto json_record = parseFromJson<ConditionRecord>(json_input, use_json);
    auto protobuf_record = parseFromJson<ConditionRecord>(json_input.dump(), use_protobuf);

    ASSERT_TRUE(json_record.has_value());
    ASSERT_TRUE(protobuf_record.has_value());
    expectEqual(*json_record, *protobuf_record);
}

TEST_F(UnitTest, ConditionRecord_ParseToJson_RoundTripAcrossParsers)
{
    ConditionRecord record = makeConditionRecordSample();

    auto json_out = parseToJson(record, use_json);
    auto protobuf_out = parseToJson(record, use_protobuf);

    ASSERT_TRUE(json_out.has_value());
    ASSERT_TRUE(protobuf_out.has_value());

    auto from_json_via_protobuf = parseFromJson<ConditionRecord>(json_out->dump(), use_protobuf);
    auto from_protobuf_via_json = parseFromJson<ConditionRecord>(json::parse(*protobuf_out), use_json);

    ASSERT_TRUE(from_json_via_protobuf.has_value());
    ASSERT_TRUE(from_protobuf_via_json.has_value());
    expectEqual(record, *from_json_via_protobuf);
    expectEqual(record, *from_protobuf_via_json);
}

TEST_F(UnitTest, Condition_ParseToJson_UseJsonDropsSolSrcExposesArgsCount)
{
    Condition condition = makeConditionSample();
    condition.set_args_count(2);

    auto json_out = parseToJson(condition, use_json);
    ASSERT_TRUE(json_out.has_value());

    EXPECT_FALSE(json_out->contains("sol_src"));
    ASSERT_TRUE(json_out->contains("args_count"));
    EXPECT_EQ((*json_out)["args_count"].get<std::uint32_t>(), 2u);
}

TEST_F(UnitTest, Condition_UseProtobuf_PreservesSolSrcForLocalStorage)
{
    Condition condition = makeConditionSample();

    auto protobuf_out = parseToJson(condition, use_protobuf);
    ASSERT_TRUE(protobuf_out.has_value());

    auto parsed = parseFromJson<Condition>(*protobuf_out, use_protobuf);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->sol_src(), condition.sol_src());
}

TEST_F(UnitTest, Condition_RuntimeCode_IsNullInTheResponseAndAbsentFromDefinitionsWhenUnset)
{
    Condition condition = makeConditionSample();

    // Unset, the response carries null and a definition bound into an artifact hash is unchanged.
    const auto plain_json = parseToJson(condition, use_json);
    const auto plain_protobuf = parseToJson(condition, use_protobuf);
    ASSERT_TRUE(plain_json.has_value());
    ASSERT_TRUE(plain_protobuf.has_value());
    ASSERT_TRUE(plain_json->contains("runtime_code"));
    EXPECT_TRUE((*plain_json)["runtime_code"].is_null());
    EXPECT_EQ(plain_protobuf->find("runtime_code"), std::string::npos);

    condition.set_runtime_code("0x6001");
    const auto coded = parseToJson(condition, use_json);
    ASSERT_TRUE(coded.has_value());
    EXPECT_EQ((*coded)["runtime_code"], "0x6001");
}

TEST_F(UnitTest, Condition_ConstructSolidityCode_UsesConstructorPattern)
{
    Condition condition = makeConditionSample();
    auto solidity_result = constructConditionSolidityCode(condition);
    ASSERT_TRUE(solidity_result.has_value());
    const std::string & solidity = *solidity_result;

    // ConditionBase(name, argc, contentHash, metadataHash) binds the published hashes.
    EXPECT_NE(solidity.find(
        "constructor(bytes32 contentHash_, bytes32 metadataHash_) "
        "ConditionBase(\"condition_check\",0, contentHash_, metadataHash_){}"), std::string::npos);
    EXPECT_EQ(solidity.find("function initialize(address registryAddr) external initializer"), std::string::npos);
    EXPECT_EQ(solidity.find("__ConditionBase_init"), std::string::npos);
}

TEST_F(UnitTest, Condition_ConstructSolidityCode_RejectsInvalidContractIdentifier)
{
    Condition condition = makeConditionSample();
    condition.set_name("bad name");

    const auto solidity_result = constructConditionSolidityCode(condition);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Condition_ConstructSolidityCode_RejectsReservedKeywordContractIdentifier)
{
    Condition condition = makeConditionSample();
    condition.set_name("function");

    const auto solidity_result = constructConditionSolidityCode(condition);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Condition_ConstructSolidityCode_RejectsArgIndexAtUint32MaxBoundary)
{
    Condition condition = makeConditionSample();
    condition.set_sol_src("return args[4294967295] == 0;");

    const auto solidity_result = constructConditionSolidityCode(condition);
    ASSERT_FALSE(solidity_result.has_value());
    EXPECT_EQ(solidity_result.error().kind, ParseError::Kind::INVALID_VALUE);
}

TEST_F(UnitTest, Condition_ConstructSolidityCode_AllowsArgIndexOneBelowUint32MaxBoundary)
{
    Condition condition = makeConditionSample();
    condition.set_sol_src("return args[4294967294] == 0;");

    const auto solidity_result = constructConditionSolidityCode(condition);
    ASSERT_TRUE(solidity_result.has_value());
    EXPECT_NE(solidity_result->find("ConditionBase(\"condition_check\",4294967295, contentHash_, metadataHash_)"), std::string::npos);
}
