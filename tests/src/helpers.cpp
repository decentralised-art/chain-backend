#include "unit-tests.hpp"

#include <sqlite3.h>

#include "sqlite/column.hpp"
#include "sqlite/exec.hpp"
#include "sqlite/statement.hpp"

using namespace dcn;
using namespace dcn::tests;

TEST_F(UnitTest, Parse_ParseUint64Decimal_AcceptsFullRange)
{
    EXPECT_EQ(parse::parseUint64Decimal("0"), 0u);
    EXPECT_EQ(parse::parseUint64Decimal("18446744073709551615"), UINT64_MAX);
}

TEST_F(UnitTest, Parse_ParseUint64Decimal_RejectsMalformedInput)
{
    for(const char * value : {"", "-1", "12a", " 1", "0x10"})
    {
        const auto parsed = parse::parseUint64Decimal(value);
        ASSERT_FALSE(parsed.has_value()) << value;
        EXPECT_EQ(parsed.error().kind, parse::ParseError::Kind::INVALID_VALUE) << value;
    }

    const auto overflow = parse::parseUint64Decimal("18446744073709551616");
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().kind, parse::ParseError::Kind::OUT_OF_RANGE);
}

TEST_F(UnitTest, Utils_TrimView_StripsSurroundingWhitespace)
{
    EXPECT_EQ(utils::trimView(" \t a b \r\n"), "a b");
    EXPECT_EQ(utils::trimView(" \t\r\n"), "");
    EXPECT_EQ(utils::trimAsciiWhitespace("  x "), std::optional<std::string>("x"));
    EXPECT_FALSE(utils::trimAsciiWhitespace("   ").has_value());
}

TEST_F(UnitTest, Utils_EqualsIgnoreCase_ComparesPrefixViews)
{
    const std::string_view url = "HTTPS://host";
    EXPECT_TRUE(utils::equalsIgnoreCase(url.substr(0, 8), "https://"));
    EXPECT_FALSE(utils::equalsIgnoreCase(url.substr(0, 7), "http://"));
    EXPECT_FALSE(utils::equalsIgnoreCase(std::string_view("http").substr(0, 8), "https://"));
}

TEST_F(UnitTest, Utils_UtcTimestamp_FormatsRfc3339TruncatedToTheSecond)
{
    using namespace std::chrono;
    EXPECT_EQ(utils::utcTimestamp(system_clock::time_point{}), "1970-01-01T00:00:00Z");
    EXPECT_EQ(utils::utcTimestamp(sys_days{2026y / October / 4} + 12h + 5min + 9s + 999ms), "2026-10-04T12:05:09Z");
    EXPECT_EQ(utils::utcTimestamp(sys_days{2024y / February / 29} + 23h + 59min + 59s), "2024-02-29T23:59:59Z");
}

TEST_F(UnitTest, Parse_DumpJsonLossy_ReplacesInvalidUtf8)
{
    EXPECT_EQ(parse::dumpJsonLossy(json{{"a", 1}}), R"({"a":1})");
    const std::string dumped = parse::dumpJsonLossy(json{{"name", std::string("ok\xFF")}});
    EXPECT_EQ(dumped, "{\"name\":\"ok\xEF\xBF\xBD\"}");
}

TEST_F(UnitTest, StorageSqlite_OptionalColumns_RoundTripNull)
{
    sqlite3 * db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(storage::sqlite::exec(db, "CREATE TABLE t (i INTEGER, s TEXT)"));

    {
        storage::sqlite::Statement insert(db, "INSERT INTO t (i, s) VALUES (?, ?)");
        ASSERT_EQ(storage::sqlite::bindOptionalInt64(insert.get(), 1, 42), SQLITE_OK);
        ASSERT_EQ(storage::sqlite::bindOptionalText(insert.get(), 2, std::string("x")), SQLITE_OK);
        ASSERT_EQ(insert.step(), SQLITE_DONE);
        insert.reset();
        ASSERT_EQ(storage::sqlite::bindOptionalInt64(insert.get(), 1, std::nullopt), SQLITE_OK);
        ASSERT_EQ(storage::sqlite::bindOptionalText(insert.get(), 2, std::nullopt), SQLITE_OK);
        ASSERT_EQ(insert.step(), SQLITE_DONE);
    }

    {
        storage::sqlite::Statement select(db, "SELECT i, s FROM t ORDER BY rowid");
        ASSERT_EQ(select.step(), SQLITE_ROW);
        EXPECT_EQ(storage::sqlite::columnInt64Optional(select.get(), 0), std::optional<std::int64_t>(42));
        EXPECT_EQ(storage::sqlite::columnTextOptional(select.get(), 1), std::optional<std::string>("x"));
        ASSERT_EQ(select.step(), SQLITE_ROW);
        EXPECT_FALSE(storage::sqlite::columnInt64Optional(select.get(), 0).has_value());
        EXPECT_FALSE(storage::sqlite::columnTextOptional(select.get(), 1).has_value());
    }

    sqlite3_close(db);
}
