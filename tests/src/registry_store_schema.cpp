#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <sqlite3.h>

#include "sqlite/exec.hpp"
#include "sqlite_registry_store.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

namespace
{
    struct Database
    {
        sqlite3 * handle = nullptr;

        explicit Database(const std::filesystem::path & path)
        {
            std::filesystem::create_directories(path.parent_path());
            sqlite3_open(path.string().c_str(), &handle);
        }

        ~Database()
        {
            sqlite3_close(handle);
        }

        std::int64_t userVersion() const
        {
            sqlite3_stmt * statement = nullptr;
            sqlite3_prepare_v2(handle, "PRAGMA user_version;", -1, &statement, nullptr);
            sqlite3_step(statement);
            const std::int64_t version = sqlite3_column_int64(statement, 0);
            sqlite3_finalize(statement);
            return version;
        }
    };

    registry::EntitySource sourceOf(const std::string & source_id)
    {
        registry::EntitySource source;
        source.source_id = source_id;
        source.chain_namespace = "eip155:1";
        source.chain_id = 1;
        source.registry_address = hexAddress(0xAB);
        return source;
    }

    std::vector<std::string> ownedTransformations(const registry::SQLiteRegistryStore & store, const std::uint8_t owner)
    {
        return store.getOwnedTransformationsCursor(makeAddressFromByte(owner), std::nullopt, 16).entries;
    }
}

TEST_F(UnitTest, Storage_EnsureSchemaVersion_StampsANewDatabaseAndRefusesAnyOther)
{
    const auto paths = makeTempEventsPaths("schema_version_guard");

    {
        Database db(paths.root / "fresh.sqlite");
        ASSERT_TRUE(storage::sqlite::ensureSchemaVersion(db.handle, 3, "test"));
        EXPECT_EQ(db.userVersion(), 3);
        EXPECT_TRUE(storage::sqlite::ensureSchemaVersion(db.handle, 3, "test"));
        EXPECT_FALSE(storage::sqlite::ensureSchemaVersion(db.handle, 4, "test"));
        EXPECT_EQ(db.userVersion(), 3);
    }

    // Tables without a version stamp predate versioning: their layout is unknown.
    {
        Database db(paths.root / "unversioned.sqlite");
        ASSERT_EQ(sqlite3_exec(db.handle, "CREATE TABLE legacy(x INTEGER);", nullptr, nullptr, nullptr), SQLITE_OK);
        EXPECT_FALSE(storage::sqlite::ensureSchemaVersion(db.handle, 1, "test"));
    }
}

TEST_F(UnitTest, SQLiteRegistryStore_RefusesADatabaseOfAnotherSchemaVersion)
{
    const auto paths = makeTempEventsPaths("registry_schema_version");
    const auto db_path = paths.root / "registry.sqlite";
    std::filesystem::create_directories(paths.root);

    {
        registry::SQLiteRegistryStore store(db_path.string());
    }
    {
        Database db(db_path);
        ASSERT_EQ(sqlite3_exec(db.handle, "PRAGMA user_version=99;", nullptr, nullptr, nullptr), SQLITE_OK);
    }

    EXPECT_THROW(registry::SQLiteRegistryStore store(db_path.string()), std::runtime_error);
}

TEST_F(UnitTest, SQLiteRegistryStore_ReprojectedOwnerReplacesTheOwnedIndexRow)
{
    const auto paths = makeTempEventsPaths("registry_owned_rows");
    std::filesystem::create_directories(paths.root);
    registry::SQLiteRegistryStore store((paths.root / "registry.sqlite").string());

    TransformationRecord record;
    record.set_owner(chain::addressToHex(makeAddressFromByte(0x0A)));
    record.mutable_transformation()->set_name("Moved");
    ASSERT_TRUE(store.addTransformationVersion(sourceOf("eip155:1:reg"), makeAddressFromByte(0x61), record));
    EXPECT_EQ(ownedTransformations(store, 0x0A), (std::vector<std::string>{"Moved"}));

    // A reorg replays the same registration with another owner.
    record.set_owner(chain::addressToHex(makeAddressFromByte(0x0B)));
    ASSERT_TRUE(store.addTransformationVersion(sourceOf("eip155:1:reg"), makeAddressFromByte(0x61), record));
    EXPECT_TRUE(ownedTransformations(store, 0x0A).empty());
    EXPECT_EQ(ownedTransformations(store, 0x0B), (std::vector<std::string>{"Moved"}));

    // Another source's version of the name keeps its own owner row.
    record.set_owner(chain::addressToHex(makeAddressFromByte(0x0A)));
    ASSERT_TRUE(store.addTransformationVersion(sourceOf("eip155:1:other"), makeAddressFromByte(0x62), record));
    EXPECT_EQ(ownedTransformations(store, 0x0A), (std::vector<std::string>{"Moved"}));
    EXPECT_EQ(ownedTransformations(store, 0x0B), (std::vector<std::string>{"Moved"}));

    ConnectorRecord connector;
    connector.set_owner(chain::addressToHex(makeAddressFromByte(0x0A)));
    connector.mutable_connector()->set_name("MovedConnector");
    connector.mutable_connector()->add_dimensions();
    ASSERT_TRUE(store.addConnectorVersion(sourceOf("eip155:1:reg"), makeAddressFromByte(0x63), connector, evmc::bytes32{}, {}));
    connector.set_owner(chain::addressToHex(makeAddressFromByte(0x0B)));
    ASSERT_TRUE(store.addConnectorVersion(sourceOf("eip155:1:reg"), makeAddressFromByte(0x63), connector, evmc::bytes32{}, {}));
    EXPECT_TRUE(store.getOwnedConnectorsCursor(makeAddressFromByte(0x0A), std::nullopt, 16).entries.empty());
    EXPECT_EQ(
        store.getOwnedConnectorsCursor(makeAddressFromByte(0x0B), std::nullopt, 16).entries,
        (std::vector<std::string>{"MovedConnector"}));
}
