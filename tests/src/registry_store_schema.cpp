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

TEST_F(UnitTest, SQLiteRegistryStore_RuntimeCode_IsStoredOnlyUnderItsOwnHash)
{
    const auto paths = makeTempEventsPaths("runtime_code_store");
    registry::SQLiteRegistryStore store((paths.root / "registry.sqlite").string());

    const std::vector<std::uint8_t> code{0x60, 0x00, 0x60, 0x00};
    const std::string code_hash = chain::bytes32ToHex(chain::keccakBytes(code.data(), code.size()));
    const std::string other_hash = chain::bytes32ToHex(chain::keccakString("other"));

    EXPECT_FALSE(store.getRuntimeCode(code_hash).has_value());
    EXPECT_FALSE(store.hasRuntimeCode(code_hash));
    EXPECT_FALSE(store.putRuntimeCode(other_hash, code));
    EXPECT_FALSE(store.putRuntimeCode(chain::bytes32ToHex(chain::keccakString("")), {}));

    EXPECT_TRUE(store.putRuntimeCode(code_hash, code));
    EXPECT_TRUE(store.putRuntimeCode(code_hash, code));
    const auto stored = store.getRuntimeCode(code_hash);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, code);
    EXPECT_TRUE(store.hasRuntimeCode(code_hash));
    EXPECT_FALSE(store.getRuntimeCode(other_hash).has_value());
    EXPECT_FALSE(store.hasRuntimeCode(other_hash));
}

// A local entity rebuilt with other code keeps its definition; its version takes the new
// hashes.
TEST_F(UnitTest, SQLiteRegistryStore_UpdateLocalVersionHashes_FollowsRebuiltCode)
{
    const auto paths = makeTempEventsPaths("local_version_hashes");
    registry::SQLiteRegistryStore store((paths.root / "registry.sqlite").string());

    const auto sourceWith = [](const std::string & source_id, const std::string & seed)
    {
        registry::EntitySource source = sourceOf(source_id);
        source.content_hash = chain::bytes32ToHex(chain::keccakString(seed + " content"));
        source.metadata_hash = chain::bytes32ToHex(chain::keccakString(seed + " metadata"));
        source.runtime_code_hash = chain::bytes32ToHex(chain::keccakString(seed + " runtime"));
        return source;
    };

    TransformationRecord record;
    record.mutable_transformation()->set_name("Rebuilt");
    record.set_owner(chain::addressToHex(makeAddressFromByte(0x0A)));
    const auto old_build = sourceWith("local", "old");
    ASSERT_TRUE(store.addTransformationVersion(old_build, makeAddressFromByte(0x61), record));

    const auto new_build = sourceWith("local", "new");
    ASSERT_TRUE(store.updateLocalVersionHashes(registry::EntityKind::TRANSFORMATION, "Rebuilt", new_build));
    const auto location = store.getEntityLocation(registry::EntityKind::TRANSFORMATION, "Rebuilt");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->source.content_hash, new_build.content_hash);
    EXPECT_EQ(location->source.metadata_hash, new_build.metadata_hash);
    EXPECT_EQ(location->source.runtime_code_hash, new_build.runtime_code_hash);

    // An event without hashes, or a name with no local version, changes nothing.
    registry::EntitySource no_hashes = sourceOf("local");
    EXPECT_TRUE(store.updateLocalVersionHashes(registry::EntityKind::TRANSFORMATION, "Rebuilt", no_hashes));
    EXPECT_TRUE(store.updateLocalVersionHashes(registry::EntityKind::TRANSFORMATION, "Unknown", new_build));
    EXPECT_EQ(store.getEntityLocation(registry::EntityKind::TRANSFORMATION, "Rebuilt")->source.content_hash,
        new_build.content_hash);

    // A chain version of the same name is never touched.
    const auto chain_build = sourceWith("eip155:1:reg", "chain");
    ASSERT_TRUE(store.addTransformationVersion(chain_build, makeAddressFromByte(0x62), record));
    ASSERT_TRUE(store.updateLocalVersionHashes(registry::EntityKind::TRANSFORMATION, "Rebuilt", sourceWith("local", "newer")));
    EXPECT_EQ(store.getEntityLocation(registry::EntityKind::TRANSFORMATION, "Rebuilt")->source.content_hash,
        chain_build.content_hash);
}

TEST_F(UnitTest, SQLiteRegistryStore_SourceSelectionScopesReadsAndRetainsDeployments)
{
    const auto paths = makeTempEventsPaths("registry_source_selection");
    const auto db_path = (paths.root / "registry.sqlite").string();
    const auto first = sourceOf("eip155:1:first");
    const auto second = sourceOf("eip155:1:second");
    const auto local_hash = chain::keccakString("local format");
    const auto first_hash = chain::keccakString("first format");
    const auto second_hash = chain::keccakString("second format");
    const auto local_owner = makeAddressFromByte(0x01);
    const auto first_owner = makeAddressFromByte(0x02);
    const auto second_owner = makeAddressFromByte(0x03);

    {
        registry::SQLiteRegistryStore store(db_path);
        const auto add = [&](registry::EntitySource source, const std::string & prefix,
                             const chain::Address & owner, const evmc::bytes32 & format_hash)
        {
            source.content_hash = chain::bytes32ToHex(chain::keccakString(prefix));
            ConnectorRecord connector;
            connector.set_owner(chain::addressToHex(owner));
            connector.mutable_connector()->set_name(prefix + "Connector");
            connector.mutable_connector()->add_dimensions();
            ASSERT_TRUE(store.addConnectorVersion(source, owner, connector, format_hash, {}));
            TransformationRecord transformation;
            transformation.set_owner(connector.owner());
            transformation.mutable_transformation()->set_name(prefix + "Transformation");
            ASSERT_TRUE(store.addTransformationVersion(source, owner, transformation));
            ConditionRecord condition;
            condition.set_owner(connector.owner());
            condition.mutable_condition()->set_name(prefix + "Condition");
            ASSERT_TRUE(store.addConditionVersion(source, owner, condition));
        };
        add({}, "Local", local_owner, local_hash);
        add(first, "Shared", first_owner, first_hash);
        add(second, "Shared", second_owner, second_hash);
        add(first, "Retained", first_owner, first_hash);
        ASSERT_TRUE(store.removeEntityVersion(registry::EntityKind::CONDITION, first.source_id, "RetainedCondition"));
        ASSERT_TRUE(store.setMaterializationCursor(123));
        ASSERT_TRUE(store.selectRemoteSource(second.source_id));

        const auto location = store.getEntityLocation(registry::EntityKind::CONNECTOR, "SharedConnector");
        ASSERT_TRUE(location);
        EXPECT_EQ(location->source.source_id, second.source_id);
        EXPECT_EQ(store.getConnectorFormatHash("SharedConnector"), second_hash);
        const auto connector = store.getConnectorRecordHandle("SharedConnector");
        const auto transformation = store.getTransformationRecordHandle("SharedTransformation");
        const auto condition = store.getConditionRecordHandle("SharedCondition");
        ASSERT_TRUE(connector && transformation && condition);
        EXPECT_EQ(chain::normalizeHex((*connector)->owner()), chain::addressToHex(second_owner));
        EXPECT_EQ(chain::normalizeHex((*transformation)->owner()), chain::addressToHex(second_owner));
        EXPECT_EQ(chain::normalizeHex((*condition)->owner()), chain::addressToHex(second_owner));
        EXPECT_TRUE(store.hasConnector("LocalConnector"));
        EXPECT_TRUE(store.hasTransformation("LocalTransformation"));
        EXPECT_TRUE(store.hasCondition("LocalCondition"));
        EXPECT_FALSE(store.hasConnector("RetainedConnector"));
        EXPECT_FALSE(store.hasTransformation("RetainedTransformation"));
        EXPECT_FALSE(store.hasRemoteEntity(registry::EntityKind::TRANSFORMATION, "RetainedTransformation"));
        EXPECT_TRUE(store.hasRemoteEntity(registry::EntityKind::TRANSFORMATION, "SharedTransformation"));

        EXPECT_EQ(store.getFormatsCount(), 2u);
        EXPECT_EQ(store.getFormatsCursor(std::nullopt, 16).entries.size(), 2u);
        EXPECT_EQ(store.getFormatsCursor(evmc::bytes32{}, 16).entries.size(), 2u);
        EXPECT_EQ(store.getFormatConnectorNamesCount(first_hash), 0u);
        EXPECT_EQ(store.getFormatConnectorNamesCount(second_hash), 1u);
        EXPECT_TRUE(store.getFormatConnectorNamesCursor(first_hash, std::nullopt, 16).entries.empty());
        EXPECT_EQ(store.getFormatConnectorNamesCursor(second_hash, std::string{}, 16).entries,
                  (std::vector<std::string>{"SharedConnector"}));
        EXPECT_TRUE(store.getOwnedConnectorsCursor(first_owner, std::nullopt, 16).entries.empty());
        EXPECT_TRUE(store.getOwnedTransformationsCursor(first_owner, std::string{}, 16).entries.empty());
        EXPECT_TRUE(store.getOwnedConditionsCursor(first_owner, std::nullopt, 16).entries.empty());
        EXPECT_EQ(store.getOwnedConditionsCursor(second_owner, std::nullopt, 16).entries,
                  (std::vector<std::string>{"SharedCondition"}));
        EXPECT_EQ(store.getAccountsCount(), 2u);
        EXPECT_EQ(store.getAccountsCursor(std::nullopt, 16).entries,
                  (std::vector<std::string>{evmc::hex(local_owner), evmc::hex(second_owner)}));
        EXPECT_EQ(store.getAccountsCursor(local_owner, 16).entries,
                  (std::vector<std::string>{evmc::hex(second_owner)}));
        EXPECT_EQ(store.getMaterializationCursor(), 123);

        ASSERT_TRUE(store.selectRemoteSource(""));
        EXPECT_FALSE(store.hasConnector("SharedConnector"));
        EXPECT_TRUE(store.hasConnector("LocalConnector"));
        EXPECT_EQ(store.getAccountsCount(), 1u);
    }

    registry::SQLiteRegistryStore reopened(db_path);
    ASSERT_TRUE(reopened.selectRemoteSource(first.source_id));
    EXPECT_TRUE(reopened.hasConnector("RetainedConnector"));
    EXPECT_FALSE(reopened.hasCondition("RetainedCondition"));
    const auto restored = reopened.getTransformationRecordHandle("SharedTransformation");
    ASSERT_TRUE(restored);
    EXPECT_EQ(chain::normalizeHex((*restored)->owner()), chain::addressToHex(first_owner));
    EXPECT_EQ(reopened.getMaterializationCursor(), 123);
}
