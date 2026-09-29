#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <stdexcept>
#include <string>

#include <evmc/hex.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include "sqlite/statement.hpp"
#include "sqlite/exec.hpp"

#include "sqlite_registry_store.hpp"

namespace dcn::registry
{
    // Bumped whenever a table definition changes; a database of another version is refused.
    constexpr int REGISTRY_SCHEMA_VERSION = 1;

    namespace
    {
        constexpr std::size_t ADDRESS_BYTES_SIZE = 20;
        constexpr std::size_t BYTES32_SIZE = 32;
        constexpr int SQLITE_DEFAULT_BUSY_TIMEOUT_MS = 5000;
        constexpr int SQLITE_CHECKPOINT_BUSY_TIMEOUT_MS = 250;

        static int toSqliteInt(std::size_t value)
        {
            constexpr std::size_t SQLITE_INT_MAX = static_cast<std::size_t>(std::numeric_limits<int>::max());
            return static_cast<int>(std::min(value, SQLITE_INT_MAX));
        }

        static int bindAddress(sqlite3_stmt * stmt, int index, const chain::Address & address)
        {
            return sqlite3_bind_blob(stmt, index, address.bytes, static_cast<int>(ADDRESS_BYTES_SIZE), SQLITE_TRANSIENT);
        }

        static int bindBytes32(sqlite3_stmt * stmt, int index, const evmc::bytes32 & value)
        {
            return sqlite3_bind_blob(stmt, index, value.bytes, static_cast<int>(BYTES32_SIZE), SQLITE_TRANSIENT);
        }

        static std::optional<evmc::bytes32> columnBytes32(sqlite3_stmt * stmt, int index)
        {
            const void * blob = sqlite3_column_blob(stmt, index);
            const int bytes = sqlite3_column_bytes(stmt, index);
            if(blob == nullptr || bytes != static_cast<int>(BYTES32_SIZE))
            {
                return std::nullopt;
            }

            evmc::bytes32 out{};
            std::memcpy(out.bytes, blob, BYTES32_SIZE);
            return out;
        }

        static std::optional<chain::Address> columnAddress(sqlite3_stmt * stmt, int index)
        {
            const void * blob = sqlite3_column_blob(stmt, index);
            const int bytes = sqlite3_column_bytes(stmt, index);
            if(blob == nullptr || bytes != static_cast<int>(ADDRESS_BYTES_SIZE))
            {
                return std::nullopt;
            }

            chain::Address out{};
            std::memcpy(out.bytes, blob, ADDRESS_BYTES_SIZE);
            return out;
        }

        static const char * entityTable(const EntityKind kind)
        {
            switch(kind)
            {
            case EntityKind::CONNECTOR: return "connectors";
            case EntityKind::TRANSFORMATION: return "transformations";
            case EntityKind::CONDITION: return "conditions";
            }
            std::terminate();
        }

        static const char * ownedTable(const EntityKind kind)
        {
            switch(kind)
            {
            case EntityKind::CONNECTOR: return "owned_connectors";
            case EntityKind::TRANSFORMATION: return "owned_transformations";
            case EntityKind::CONDITION: return "owned_conditions";
            }
            std::terminate();
        }

        static pt::ArtifactState parseArtifactState(const char * value)
        {
            const std::string_view state = value == nullptr ? std::string_view{} : value;
            if(state == "available") return pt::ArtifactState::AVAILABLE;
            if(state == "missing") return pt::ArtifactState::MISSING;
            return pt::ArtifactState::INVALID;
        }
    }

    SQLiteRegistryStore::SQLiteRegistryStore(const std::string & db_path)
    {
        const int open_res = sqlite3_open_v2(
            db_path.empty() ? ":memory:" : db_path.c_str(),
            &_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if(open_res != SQLITE_OK)
        {
            const std::string msg = (_db == nullptr) ? "sqlite open failed" : sqlite3_errmsg(_db);
            if(_db != nullptr)
            {
                sqlite3_close(_db);
                _db = nullptr;
            }

            throw std::runtime_error(msg);
        }

        sqlite3_busy_timeout(_db, SQLITE_DEFAULT_BUSY_TIMEOUT_MS);

        if(!_exec("PRAGMA journal_mode=WAL;") || !_exec("PRAGMA synchronous=NORMAL;") ||
            !_exec("PRAGMA temp_store=MEMORY;") || !_exec("PRAGMA foreign_keys=OFF;"))
        {
            throw std::runtime_error("Failed to configure SQLite pragmas");
        }

        if(!storage::sqlite::ensureSchemaVersion(_db, REGISTRY_SCHEMA_VERSION, "registry") || !_initializeSchema())
        {
            throw std::runtime_error("Failed to initialize SQLite registry schema");
        }
    }

    SQLiteRegistryStore::~SQLiteRegistryStore()
    {
        if(_db != nullptr)
        {
            sqlite3_close(_db);
            _db = nullptr;
        }
    }

    bool SQLiteRegistryStore::_exec(const char * sql) const
    {
        return storage::sqlite::exec(_db, sql);
    }

    bool SQLiteRegistryStore::_beginTransaction() const
    {
        return _exec("BEGIN IMMEDIATE TRANSACTION;");
    }

    bool SQLiteRegistryStore::_commitTransaction() const
    {
        return _exec("COMMIT;");
    }

    void SQLiteRegistryStore::_rollbackTransaction() const
    {
        (void)_exec("ROLLBACK;");
    }

    bool SQLiteRegistryStore::_initializeSchema() const
    {
        const bool base_schema_ok =
            // Entities mirror chain-derivable state in explicit columns. Executable source stays
            // in the verified content-addressed artifact referenced by content_hash.
            _exec(
                "CREATE TABLE IF NOT EXISTS connectors ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "chain_namespace TEXT NOT NULL DEFAULT 'local',"
                "chain_id INTEGER NOT NULL DEFAULT 1,"
                "registry_address TEXT NOT NULL DEFAULT '0x0',"
                "entity_address BLOB NOT NULL DEFAULT (zeroblob(20)),"
                "name TEXT NOT NULL,"
                "owner BLOB NOT NULL,"
                "format_hash BLOB NOT NULL,"
                "condition_name TEXT NOT NULL,"
                "content_hash TEXT NOT NULL DEFAULT '',"
                "metadata_hash TEXT NOT NULL DEFAULT '',"
                "runtime_code_hash TEXT NOT NULL DEFAULT '',"
                "artifact_state TEXT NOT NULL DEFAULT 'missing' CHECK(artifact_state IN ('available','missing','invalid')) ,"
                "artifact_attempts INTEGER NOT NULL DEFAULT 0,"
                "artifact_retry_at_ms INTEGER NOT NULL DEFAULT 0,"
                "definition_conflict INTEGER NOT NULL DEFAULT 0,"
                "active INTEGER NOT NULL DEFAULT 1,"
                "origin_block_hash TEXT NOT NULL DEFAULT '',"
                "origin_log_index INTEGER NOT NULL DEFAULT 0,"
                "created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER))"
                ",PRIMARY KEY(source_id, name)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS connector_dimensions ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "connector_name TEXT NOT NULL,"
                "dim_index INTEGER NOT NULL,"
                "composite TEXT NOT NULL,"
                "PRIMARY KEY(source_id, connector_name, dim_index)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS connector_dimension_bindings ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "connector_name TEXT NOT NULL,"
                "dim_index INTEGER NOT NULL,"
                "slot TEXT NOT NULL,"
                "target TEXT NOT NULL,"
                "PRIMARY KEY(source_id, connector_name, dim_index, slot)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS connector_transformation_defs ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "connector_name TEXT NOT NULL,"
                "dim_index INTEGER NOT NULL,"
                "op_index INTEGER NOT NULL,"
                "transformation_name TEXT NOT NULL,"
                "PRIMARY KEY(source_id, connector_name, dim_index, op_index)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS connector_transformation_def_args ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "connector_name TEXT NOT NULL,"
                "dim_index INTEGER NOT NULL,"
                "op_index INTEGER NOT NULL,"
                "arg_index INTEGER NOT NULL,"
                "value INTEGER NOT NULL,"
                "PRIMARY KEY(source_id, connector_name, dim_index, op_index, arg_index)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS connector_condition_args ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "connector_name TEXT NOT NULL,"
                "arg_index INTEGER NOT NULL,"
                "value INTEGER NOT NULL,"
                "PRIMARY KEY(source_id, connector_name, arg_index)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS connector_static_ri ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "connector_name TEXT NOT NULL,"
                "position INTEGER NOT NULL,"
                "start_point INTEGER NOT NULL,"
                "transformation_shift INTEGER NOT NULL,"
                "PRIMARY KEY(source_id, connector_name, position)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS transformations ("
                "source_id TEXT NOT NULL DEFAULT 'local', chain_namespace TEXT NOT NULL DEFAULT 'local', chain_id INTEGER NOT NULL DEFAULT 1,"
                "registry_address TEXT NOT NULL DEFAULT '0x0', entity_address BLOB NOT NULL DEFAULT (zeroblob(20)), name TEXT NOT NULL,"
                "owner BLOB NOT NULL,"
                "args_count INTEGER NOT NULL,"
                "content_hash TEXT NOT NULL DEFAULT '', metadata_hash TEXT NOT NULL DEFAULT '',"
                "runtime_code_hash TEXT NOT NULL DEFAULT '',"
                "artifact_state TEXT NOT NULL DEFAULT 'missing' CHECK(artifact_state IN ('available','missing','invalid')) ,"
                "artifact_attempts INTEGER NOT NULL DEFAULT 0, artifact_retry_at_ms INTEGER NOT NULL DEFAULT 0, definition_conflict INTEGER NOT NULL DEFAULT 0, active INTEGER NOT NULL DEFAULT 1,"
                "origin_block_hash TEXT NOT NULL DEFAULT '', origin_log_index INTEGER NOT NULL DEFAULT 0,"
                "created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER))"
                ",PRIMARY KEY(source_id, name)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS conditions ("
                "source_id TEXT NOT NULL DEFAULT 'local', chain_namespace TEXT NOT NULL DEFAULT 'local', chain_id INTEGER NOT NULL DEFAULT 1,"
                "registry_address TEXT NOT NULL DEFAULT '0x0', entity_address BLOB NOT NULL DEFAULT (zeroblob(20)), name TEXT NOT NULL,"
                "owner BLOB NOT NULL,"
                "args_count INTEGER NOT NULL,"
                "content_hash TEXT NOT NULL DEFAULT '', metadata_hash TEXT NOT NULL DEFAULT '',"
                "runtime_code_hash TEXT NOT NULL DEFAULT '',"
                "artifact_state TEXT NOT NULL DEFAULT 'missing' CHECK(artifact_state IN ('available','missing','invalid')) ,"
                "artifact_attempts INTEGER NOT NULL DEFAULT 0, artifact_retry_at_ms INTEGER NOT NULL DEFAULT 0, definition_conflict INTEGER NOT NULL DEFAULT 0, active INTEGER NOT NULL DEFAULT 1,"
                "origin_block_hash TEXT NOT NULL DEFAULT '', origin_log_index INTEGER NOT NULL DEFAULT 0,"
                "created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER))"
                ",PRIMARY KEY(source_id, name)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS format_members ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "format_hash BLOB NOT NULL,"
                "name TEXT NOT NULL,"
                "PRIMARY KEY(source_id, format_hash, name)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS scalar_labels_by_format ("
                "format_hash BLOB NOT NULL,"
                "scalar TEXT NOT NULL,"
                "path_hash BLOB NOT NULL,"
                "tail_id INTEGER NOT NULL,"
                "PRIMARY KEY(format_hash, scalar, path_hash, tail_id)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS owned_connectors ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "owner BLOB NOT NULL,"
                "name TEXT NOT NULL,"
                "PRIMARY KEY(source_id, owner, name)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS owned_transformations ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "owner BLOB NOT NULL,"
                "name TEXT NOT NULL,"
                "PRIMARY KEY(source_id, owner, name)"
                ");") &&
            _exec(
                "CREATE TABLE IF NOT EXISTS owned_conditions ("
                "source_id TEXT NOT NULL DEFAULT 'local',"
                "owner BLOB NOT NULL,"
                "name TEXT NOT NULL,"
                "PRIMARY KEY(source_id, owner, name)"
                ");") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_format_members_format_name ON format_members(format_hash, name);") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_scalar_labels_format ON scalar_labels_by_format(format_hash);") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_owned_connectors_owner_name ON owned_connectors(owner, name);") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_owned_transformations_owner_name ON owned_transformations(owner, name);") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_owned_conditions_owner_name ON owned_conditions(owner, name);") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_connectors_artifact_work ON connectors(artifact_state, artifact_retry_at_ms) WHERE active=1 AND content_hash<>'';") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_transformations_artifact_work ON transformations(artifact_state, artifact_retry_at_ms) WHERE active=1 AND content_hash<>'';") &&
            _exec("CREATE INDEX IF NOT EXISTS idx_conditions_artifact_work ON conditions(artifact_state, artifact_retry_at_ms) WHERE active=1 AND content_hash<>'';");

        if(!base_schema_ok)
        {
            return false;
        }

        // Materialization cursor: singleton row that tracks the last hot-store change_seq
        // consumed by RegistryProjector. Seeded to 0 so a fresh DB starts from the beginning.
        const bool cursor_ok =
            _exec(
                "CREATE TABLE IF NOT EXISTS materialization_cursor ("
                "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                "last_change_seq INTEGER NOT NULL"
                ");") &&
            _exec("INSERT OR IGNORE INTO materialization_cursor(singleton, last_change_seq) VALUES(1, 0);");

        if(!cursor_ok)
        {
            return false;
        }

        return true;
    }

    std::int64_t SQLiteRegistryStore::getMaterializationCursor() const
    {
        try
        {
            storage::sqlite::Statement stmt(_db,
                "SELECT last_change_seq FROM materialization_cursor WHERE singleton=1;");
            if(stmt.step() != SQLITE_ROW)
            {
                return 0;
            }
            return static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 0));
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite getMaterializationCursor failed: {}", e.what());
            return 0;
        }
    }

    bool SQLiteRegistryStore::setMaterializationCursor(std::int64_t last_change_seq)
    {
        try
        {
            storage::sqlite::Statement stmt(_db,
                "UPDATE materialization_cursor SET last_change_seq=?1 WHERE singleton=1;");
            sqlite3_bind_int64(stmt.get(), 1, last_change_seq);
            if(stmt.step() != SQLITE_DONE)
            {
                spdlog::error("SQLite setMaterializationCursor seq={} did not complete: {}",
                    last_change_seq, sqlite3_errmsg(_db));
                return false;
            }
            return true;
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite setMaterializationCursor failed seq={}: {}", last_change_seq, e.what());
            return false;
        }
    }

    bool SQLiteRegistryStore::_insertConnectorRows(
        const EntitySource & source,
        const chain::Address & address,
        const chain::Address & owner,
        const evmc::bytes32 & format_hash,
        const ConnectorRecord & record,
        const std::vector<ScalarLabel> & canonical_scalar_labels) const
    {
        const Connector & connector = record.connector();
        const std::string & connector_name = connector.name();

        // Replace semantics: re-projecting the same (source_id, name) version must be
        // idempotent, so every row this function writes is cleared for that key first.
        for(const char * table : {
                "connectors",
                "connector_dimensions",
                "connector_dimension_bindings",
                "connector_transformation_defs",
                "connector_transformation_def_args",
                "connector_condition_args",
                "connector_static_ri",
                "format_members",
                "owned_connectors"})
        {
            const std::string_view table_name(table);
            const char * name_column =
                (table_name == "connectors" || table_name == "format_members" || table_name == "owned_connectors")
                    ? "name"
                    : "connector_name";
            storage::sqlite::Statement del(
                _db,
                std::format("DELETE FROM {} WHERE source_id=?1 AND {}=?2;", table, name_column).c_str());
            sqlite3_bind_text(del.get(), 1, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(del.get(), 2, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
            if(del.step() != SQLITE_DONE)
            {
                spdlog::error("SQLite delete {} failed for `{}`", table, connector_name);
                return false;
            }
        }

        {
            storage::sqlite::Statement insert_connector(
                _db,
                "INSERT INTO connectors(source_id, chain_namespace, chain_id, registry_address, entity_address, name, owner, "
                "format_hash, condition_name, content_hash, metadata_hash, runtime_code_hash, artifact_state, definition_conflict, origin_block_hash, origin_log_index) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16);");
            sqlite3_bind_text(insert_connector.get(), 1, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_connector.get(), 2, source.chain_namespace.c_str(), static_cast<int>(source.chain_namespace.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_connector.get(), 3, static_cast<sqlite3_int64>(source.chain_id));
            sqlite3_bind_text(insert_connector.get(), 4, source.registry_address.c_str(), static_cast<int>(source.registry_address.size()), SQLITE_TRANSIENT);
            bindAddress(insert_connector.get(), 5, address);
            sqlite3_bind_text(insert_connector.get(), 6, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
            bindAddress(insert_connector.get(), 7, owner);
            bindBytes32(insert_connector.get(), 8, format_hash);
            sqlite3_bind_text(insert_connector.get(), 9, connector.condition_name().c_str(), static_cast<int>(connector.condition_name().size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_connector.get(), 10, source.content_hash.c_str(), static_cast<int>(source.content_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_connector.get(), 11, source.metadata_hash.c_str(), static_cast<int>(source.metadata_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_connector.get(), 12, source.runtime_code_hash.c_str(), static_cast<int>(source.runtime_code_hash.size()), SQLITE_TRANSIENT);
            const std::string_view artifact_state = pt::artifactStateName(source.artifact_state);
            sqlite3_bind_text(insert_connector.get(), 13, artifact_state.data(), static_cast<int>(artifact_state.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_connector.get(), 14, source.definition_conflict ? 1 : 0);
            sqlite3_bind_text(insert_connector.get(), 15, source.block_hash.c_str(), static_cast<int>(source.block_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_connector.get(), 16, source.log_index);
            if(insert_connector.step() != SQLITE_DONE)
            {
                spdlog::error("SQLite insert connectors failed for `{}`", connector_name);
                return false;
            }
        }

        {
            storage::sqlite::Statement insert_format_member(
                _db,
                "INSERT OR IGNORE INTO format_members(format_hash, name, source_id) VALUES(?1, ?2, ?3);");
            bindBytes32(insert_format_member.get(), 1, format_hash);
            sqlite3_bind_text(insert_format_member.get(), 2, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_format_member.get(), 3, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            if(insert_format_member.step() != SQLITE_DONE)
            {
                spdlog::error("SQLite insert format_members failed for `{}`", connector_name);
                return false;
            }
        }

        {
            storage::sqlite::Statement insert_scalar_label(
                _db,
                "INSERT OR IGNORE INTO scalar_labels_by_format(format_hash, scalar, path_hash, tail_id) VALUES(?1, ?2, ?3, ?4);");
            for(const ScalarLabel & label : canonical_scalar_labels)
            {
                insert_scalar_label.reset();
                bindBytes32(insert_scalar_label.get(), 1, format_hash);
                sqlite3_bind_text(insert_scalar_label.get(), 2, label.scalar.c_str(), static_cast<int>(label.scalar.size()), SQLITE_TRANSIENT);
                bindBytes32(insert_scalar_label.get(), 3, label.path_hash);
                sqlite3_bind_int64(insert_scalar_label.get(), 4, static_cast<sqlite3_int64>(label.tail_id));
                if(insert_scalar_label.step() != SQLITE_DONE)
                {
                    spdlog::error("SQLite insert scalar_labels_by_format failed for `{}`", connector_name);
                    return false;
                }
            }
        }

        {
            storage::sqlite::Statement insert_owned(
                _db,
                "INSERT OR REPLACE INTO owned_connectors(owner, name, source_id) VALUES(?1, ?2, ?3);");
            bindAddress(insert_owned.get(), 1, owner);
            sqlite3_bind_text(insert_owned.get(), 2, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_owned.get(), 3, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            if(insert_owned.step() != SQLITE_DONE)
            {
                spdlog::error("SQLite insert owned_connectors failed for `{}`", connector_name);
                return false;
            }
        }

        {
            storage::sqlite::Statement insert_cond_arg(
                _db,
                "INSERT INTO connector_condition_args(connector_name, arg_index, value, source_id) VALUES(?1, ?2, ?3, ?4);");
            for(int i = 0; i < connector.condition_args_size(); ++i)
            {
                insert_cond_arg.reset();
                sqlite3_bind_text(insert_cond_arg.get(), 1, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(insert_cond_arg.get(), 2, static_cast<sqlite3_int64>(i));
                sqlite3_bind_int64(insert_cond_arg.get(), 3, static_cast<sqlite3_int64>(connector.condition_args(i)));
                sqlite3_bind_text(insert_cond_arg.get(), 4, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
                if(insert_cond_arg.step() != SQLITE_DONE)
                {
                    spdlog::error("SQLite insert connector_condition_args failed for `{}`", connector_name);
                    return false;
                }
            }
        }

        {
            storage::sqlite::Statement insert_static_ri(
                _db,
                "INSERT INTO connector_static_ri(connector_name, position, start_point, transformation_shift, source_id) VALUES(?1, ?2, ?3, ?4, ?5);");
            for(const auto & [position, running_instance] : connector.static_ri())
            {
                insert_static_ri.reset();
                sqlite3_bind_text(insert_static_ri.get(), 1, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(insert_static_ri.get(), 2, static_cast<sqlite3_int64>(position));
                sqlite3_bind_int64(insert_static_ri.get(), 3, static_cast<sqlite3_int64>(running_instance.start_point()));
                sqlite3_bind_int64(insert_static_ri.get(), 4, static_cast<sqlite3_int64>(running_instance.transformation_shift()));
                sqlite3_bind_text(insert_static_ri.get(), 5, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
                if(insert_static_ri.step() != SQLITE_DONE)
                {
                    spdlog::error("SQLite insert connector_static_ri failed for `{}`", connector_name);
                    return false;
                }
            }
        }

        {
            storage::sqlite::Statement insert_dim(
                _db,
                "INSERT INTO connector_dimensions(connector_name, dim_index, composite, source_id) VALUES(?1, ?2, ?3, ?4);");
            storage::sqlite::Statement insert_binding(
                _db,
                "INSERT INTO connector_dimension_bindings(connector_name, dim_index, slot, target, source_id) VALUES(?1, ?2, ?3, ?4, ?5);");
            storage::sqlite::Statement insert_tdef(
                _db,
                "INSERT INTO connector_transformation_defs(connector_name, dim_index, op_index, transformation_name, source_id) VALUES(?1, ?2, ?3, ?4, ?5);");
            storage::sqlite::Statement insert_tdef_arg(
                _db,
                "INSERT INTO connector_transformation_def_args(connector_name, dim_index, op_index, arg_index, value, source_id) VALUES(?1, ?2, ?3, ?4, ?5, ?6);");

            for(int dim_index = 0; dim_index < connector.dimensions_size(); ++dim_index)
            {
                const Dimension & dimension = connector.dimensions(dim_index);

                insert_dim.reset();
                sqlite3_bind_text(insert_dim.get(), 1, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(insert_dim.get(), 2, static_cast<sqlite3_int64>(dim_index));
                sqlite3_bind_text(insert_dim.get(), 3, dimension.composite().c_str(), static_cast<int>(dimension.composite().size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_dim.get(), 4, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
                if(insert_dim.step() != SQLITE_DONE)
                {
                    spdlog::error("SQLite insert connector_dimensions failed for `{}`", connector_name);
                    return false;
                }

                for(const auto & [slot, target] : dimension.bindings())
                {
                    insert_binding.reset();
                    sqlite3_bind_text(insert_binding.get(), 1, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_int64(insert_binding.get(), 2, static_cast<sqlite3_int64>(dim_index));
                    sqlite3_bind_text(insert_binding.get(), 3, slot.c_str(), static_cast<int>(slot.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_binding.get(), 4, target.c_str(), static_cast<int>(target.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_binding.get(), 5, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
                    if(insert_binding.step() != SQLITE_DONE)
                    {
                        spdlog::error("SQLite insert connector_dimension_bindings failed for `{}`", connector_name);
                        return false;
                    }
                }

                for(int op_index = 0; op_index < dimension.transformations_size(); ++op_index)
                {
                    const TransformationDef & tdef = dimension.transformations(op_index);

                    insert_tdef.reset();
                    sqlite3_bind_text(insert_tdef.get(), 1, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_int64(insert_tdef.get(), 2, static_cast<sqlite3_int64>(dim_index));
                    sqlite3_bind_int64(insert_tdef.get(), 3, static_cast<sqlite3_int64>(op_index));
                    sqlite3_bind_text(insert_tdef.get(), 4, tdef.name().c_str(), static_cast<int>(tdef.name().size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_tdef.get(), 5, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
                    if(insert_tdef.step() != SQLITE_DONE)
                    {
                        spdlog::error("SQLite insert connector_transformation_defs failed for `{}`", connector_name);
                        return false;
                    }

                    for(int arg_index = 0; arg_index < tdef.args_size(); ++arg_index)
                    {
                        insert_tdef_arg.reset();
                        sqlite3_bind_text(insert_tdef_arg.get(), 1, connector_name.c_str(), static_cast<int>(connector_name.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_int64(insert_tdef_arg.get(), 2, static_cast<sqlite3_int64>(dim_index));
                        sqlite3_bind_int64(insert_tdef_arg.get(), 3, static_cast<sqlite3_int64>(op_index));
                        sqlite3_bind_int64(insert_tdef_arg.get(), 4, static_cast<sqlite3_int64>(arg_index));
                        sqlite3_bind_int64(insert_tdef_arg.get(), 5, static_cast<sqlite3_int64>(tdef.args(arg_index)));
                        sqlite3_bind_text(insert_tdef_arg.get(), 6, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
                        if(insert_tdef_arg.step() != SQLITE_DONE)
                        {
                            spdlog::error("SQLite insert connector_transformation_def_args failed for `{}`", connector_name);
                            return false;
                        }
                    }
                }
            }
        }

        return true;
    }

    std::optional<ConnectorRecord> SQLiteRegistryStore::_readConnectorRecord(
        const std::string & source_id,
        const std::string & name) const
    {
        ConnectorRecord record;
        Connector * connector = record.mutable_connector();
        connector->set_name(name);

        {
            storage::sqlite::Statement stmt(_db, "SELECT owner, condition_name FROM connectors WHERE source_id=?1 AND name=?2 AND active=1;");
            sqlite3_bind_text(stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 2, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(stmt.step() != SQLITE_ROW)
            {
                return std::nullopt;
            }

            const auto owner = columnAddress(stmt.get(), 0);
            if(!owner)
            {
                spdlog::error("SQLite connector `{}` has invalid owner column", name);
                return std::nullopt;
            }
            record.set_owner(evmc::hex(*owner));

            const unsigned char * condition_name = sqlite3_column_text(stmt.get(), 1);
            if(condition_name != nullptr)
            {
                connector->set_condition_name(reinterpret_cast<const char *>(condition_name));
            }
        }

        {
            storage::sqlite::Statement stmt(_db, "SELECT value FROM connector_condition_args WHERE connector_name=?1 AND source_id=?2 ORDER BY arg_index ASC;");
            sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
            {
                connector->add_condition_args(static_cast<std::int32_t>(sqlite3_column_int64(stmt.get(), 0)));
            }
        }

        {
            storage::sqlite::Statement stmt(_db, "SELECT position, start_point, transformation_shift FROM connector_static_ri WHERE connector_name=?1 AND source_id=?2;");
            sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
            {
                const std::uint32_t position = static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 0));
                RunningInstance running_instance;
                running_instance.set_start_point(static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 1)));
                running_instance.set_transformation_shift(static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 2)));
                (*connector->mutable_static_ri())[position] = running_instance;
            }
        }

        {
            storage::sqlite::Statement dim_stmt(_db, "SELECT dim_index, composite FROM connector_dimensions WHERE connector_name=?1 AND source_id=?2 ORDER BY dim_index ASC;");
            sqlite3_bind_text(dim_stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(dim_stmt.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            for(int rc = dim_stmt.step(); rc == SQLITE_ROW; rc = dim_stmt.step())
            {
                const std::int64_t dim_index = sqlite3_column_int64(dim_stmt.get(), 0);
                Dimension * dimension = connector->add_dimensions();
                const unsigned char * composite = sqlite3_column_text(dim_stmt.get(), 1);
                if(composite != nullptr)
                {
                    dimension->set_composite(reinterpret_cast<const char *>(composite));
                }

                {
                    storage::sqlite::Statement binding_stmt(_db, "SELECT slot, target FROM connector_dimension_bindings WHERE connector_name=?1 AND dim_index=?2 AND source_id=?3;");
                    sqlite3_bind_text(binding_stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_int64(binding_stmt.get(), 2, dim_index);
                    sqlite3_bind_text(binding_stmt.get(), 3, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                    for(int brc = binding_stmt.step(); brc == SQLITE_ROW; brc = binding_stmt.step())
                    {
                        const unsigned char * slot = sqlite3_column_text(binding_stmt.get(), 0);
                        const unsigned char * target = sqlite3_column_text(binding_stmt.get(), 1);
                        if(slot != nullptr && target != nullptr)
                        {
                            (*dimension->mutable_bindings())[reinterpret_cast<const char *>(slot)] = reinterpret_cast<const char *>(target);
                        }
                    }
                }

                {
                    storage::sqlite::Statement tdef_stmt(_db, "SELECT op_index, transformation_name FROM connector_transformation_defs WHERE connector_name=?1 AND dim_index=?2 AND source_id=?3 ORDER BY op_index ASC;");
                    sqlite3_bind_text(tdef_stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_int64(tdef_stmt.get(), 2, dim_index);
                    sqlite3_bind_text(tdef_stmt.get(), 3, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                    for(int trc = tdef_stmt.step(); trc == SQLITE_ROW; trc = tdef_stmt.step())
                    {
                        const std::int64_t op_index = sqlite3_column_int64(tdef_stmt.get(), 0);
                        TransformationDef * tdef = dimension->add_transformations();
                        const unsigned char * tdef_name = sqlite3_column_text(tdef_stmt.get(), 1);
                        if(tdef_name != nullptr)
                        {
                            tdef->set_name(reinterpret_cast<const char *>(tdef_name));
                        }

                        storage::sqlite::Statement arg_stmt(_db, "SELECT value FROM connector_transformation_def_args WHERE connector_name=?1 AND dim_index=?2 AND op_index=?3 AND source_id=?4 ORDER BY arg_index ASC;");
                        sqlite3_bind_text(arg_stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_int64(arg_stmt.get(), 2, dim_index);
                        sqlite3_bind_int64(arg_stmt.get(), 3, op_index);
                        sqlite3_bind_text(arg_stmt.get(), 4, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                        for(int arc = arg_stmt.step(); arc == SQLITE_ROW; arc = arg_stmt.step())
                        {
                            tdef->add_args(static_cast<std::int32_t>(sqlite3_column_int64(arg_stmt.get(), 0)));
                        }
                    }
                }
            }
        }

        return record;
    }

    bool SQLiteRegistryStore::hasConnector(const std::string & name) const
    {
        try
        {
            if(_db == nullptr)
            {
                spdlog::error("SQLite hasConnector called with null DB handle for name={}", name);
                return false;
            }
            
            spdlog::debug("SQLite::hasConnector('{}'): prepare (db={})", name, static_cast<const void *>(_db));
            
            storage::sqlite::Statement stmt(_db, "SELECT 1 FROM connectors WHERE name=?1 AND active=1 LIMIT 1;");
            const int bind_rc = sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            
            spdlog::debug("SQLite::hasConnector('{}'): bind rc={}", name, bind_rc);
            
            if(bind_rc != SQLITE_OK)
            {
                spdlog::error("SQLite::hasConnector('{}'): bind failed rc={} err={}", name, bind_rc, sqlite3_errmsg(_db));
                return false;
            }
            const int rc = stmt.step();
            
            spdlog::debug("SQLite::hasConnector('{}'): step rc={}", name, rc);
            
            return rc == SQLITE_ROW;
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite hasConnector query failed for name={}: {}", name, e.what());
            return false;
        }
    }

    std::optional<ConnectorRecordHandle> SQLiteRegistryStore::getConnectorRecordHandle(const std::string & name) const
    {
        try
        {
            spdlog::debug("SQLite::getConnectorRecordHandle('{}'): prepare", name);
            
            storage::sqlite::Statement source_stmt(
                _db,
                "SELECT source_id FROM connectors WHERE name=?1 AND active=1 "
                "ORDER BY CASE WHEN source_id='local' THEN 1 ELSE 0 END, source_id LIMIT 1;");
            sqlite3_bind_text(source_stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(source_stmt.step() != SQLITE_ROW) return std::nullopt;
            const std::string source_id = reinterpret_cast<const char*>(sqlite3_column_text(source_stmt.get(), 0));
            auto record_opt = _readConnectorRecord(source_id, name);
            if(!record_opt.has_value())
            {
                spdlog::debug("SQLite::getConnectorRecordHandle('{}'): not found", name);
                return std::nullopt;
            }

            spdlog::debug("SQLite::getConnectorRecordHandle('{}'): reassembled record", name);
            return std::make_shared<ConnectorRecord>(std::move(*record_opt));
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite getConnectorRecordHandle query failed for name={}: {}", name, e.what());
            return std::nullopt;
        }
    }

    std::optional<evmc::bytes32> SQLiteRegistryStore::getConnectorFormatHash(const std::string & name) const
    {
        try
        {
            storage::sqlite::Statement stmt(
                _db,
                "SELECT format_hash FROM connectors WHERE name=?1 AND active=1 "
                "ORDER BY CASE WHEN source_id='local' THEN 1 ELSE 0 END, source_id LIMIT 1;");
            sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(stmt.step() != SQLITE_ROW)
            {
                return std::nullopt;
            }
            return columnBytes32(stmt.get(), 0);
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite getConnectorFormatHash query failed for name={}: {}", name, e.what());
            return std::nullopt;
        }
    }

    bool SQLiteRegistryStore::addConnector(
        const chain::Address & address,
        const ConnectorRecord & record,
        const evmc::bytes32 & format_hash,
        const std::vector<ScalarLabel> & canonical_scalar_labels)
    {
        return addConnectorVersion(EntitySource{}, address, record, format_hash, canonical_scalar_labels);
    }

    std::optional<EntityLocation> SQLiteRegistryStore::getEntityLocation(
        const EntityKind kind,
        const std::string & name) const
    {
        storage::sqlite::Statement stmt(
            _db,
            std::format(
                "SELECT source_id,chain_namespace,chain_id,registry_address,entity_address,content_hash,metadata_hash,runtime_code_hash,artifact_state,definition_conflict,origin_block_hash,origin_log_index,active "
                "FROM {} WHERE name=?1 AND active=1 "
                "ORDER BY CASE WHEN source_id='local' THEN 1 ELSE 0 END, source_id LIMIT 1;",
                entityTable(kind)).c_str());
        sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        if(stmt.step() != SQLITE_ROW) return std::nullopt;

        const auto address = columnAddress(stmt.get(), 4);
        if(!address) return std::nullopt;

        EntityLocation location{};
        location.source.source_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        location.source.chain_namespace = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        location.source.chain_id = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 2));
        location.source.registry_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        location.address = *address;
        location.source.content_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        location.source.metadata_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
        location.source.runtime_code_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        location.source.artifact_state = parseArtifactState(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8)));
        location.source.definition_conflict = sqlite3_column_int(stmt.get(), 9) != 0;
        location.source.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 10));
        location.source.log_index = sqlite3_column_int64(stmt.get(), 11);
        location.active = sqlite3_column_int(stmt.get(), 12) != 0;
        return location;
    }

    bool SQLiteRegistryStore::hasRemoteEntity(const EntityKind kind, const std::string & name) const
    {
        storage::sqlite::Statement stmt(
            _db,
            std::format("SELECT 1 FROM {} WHERE name=?1 AND source_id<>'local' AND active=1 LIMIT 1;", entityTable(kind)).c_str());
        sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        return stmt.step() == SQLITE_ROW;
    }

    bool SQLiteRegistryStore::hasLocalEntity(const EntityKind kind, const std::string & name) const
    {
        storage::sqlite::Statement stmt(
            _db,
            std::format("SELECT 1 FROM {} WHERE name=?1 AND source_id='local' AND active=1 LIMIT 1;", entityTable(kind)).c_str());
        sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        return stmt.step() == SQLITE_ROW;
    }

    bool SQLiteRegistryStore::removeEntityVersion(
        const EntityKind kind,
        const std::string & source_id,
        const std::string & name)
    {
        if(!_beginTransaction()) return false;
        try
        {
            storage::sqlite::Statement owner_stmt(
                _db,
                std::format("SELECT owner FROM {} WHERE source_id=?1 AND name=?2 AND active=1;", entityTable(kind)).c_str());
            sqlite3_bind_text(owner_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(owner_stmt.get(), 2, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(owner_stmt.step() != SQLITE_ROW)
            {
                _rollbackTransaction();
                return true;
            }
            const auto owner = columnAddress(owner_stmt.get(), 0);
            if(!owner) throw std::runtime_error("invalid entity owner");

            storage::sqlite::Statement update_stmt(
                _db,
                std::format("UPDATE {} SET active=0 WHERE source_id=?1 AND name=?2;", entityTable(kind)).c_str());
            sqlite3_bind_text(update_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(update_stmt.get(), 2, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(update_stmt.step() != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(_db));

            storage::sqlite::Statement owned_stmt(
                _db,
                std::format("DELETE FROM {} WHERE source_id=?1 AND owner=?2 AND name=?3;", ownedTable(kind)).c_str());
            sqlite3_bind_text(owned_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            bindAddress(owned_stmt.get(), 2, *owner);
            sqlite3_bind_text(owned_stmt.get(), 3, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(owned_stmt.step() != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(_db));

            if(kind == EntityKind::CONNECTOR)
            {
                storage::sqlite::Statement format_stmt(
                    _db,
                    "DELETE FROM format_members WHERE source_id=?1 AND name=?2;");
                sqlite3_bind_text(format_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(format_stmt.get(), 2, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
                if(format_stmt.step() != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(_db));
            }
            if(!_commitTransaction()) throw std::runtime_error("commit failed");
            return true;
        }
        catch(...)
        {
            _rollbackTransaction();
            return false;
        }
    }

    bool SQLiteRegistryStore::changeEntityOwner(
        const EntityKind kind,
        const std::string & source_id,
        const std::string & name,
        const chain::Address & old_owner,
        const chain::Address & new_owner)
    {
        if(!_beginTransaction()) return false;
        try
        {
            storage::sqlite::Statement update_stmt(
                _db,
                std::format("UPDATE {} SET owner=?1 WHERE source_id=?2 AND name=?3 AND owner=?4 AND active=1;", entityTable(kind)).c_str());
            bindAddress(update_stmt.get(), 1, new_owner);
            sqlite3_bind_text(update_stmt.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(update_stmt.get(), 3, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            bindAddress(update_stmt.get(), 4, old_owner);
            if(update_stmt.step() != SQLITE_DONE || sqlite3_changes(_db) != 1) throw std::runtime_error("owner mismatch");

            storage::sqlite::Statement delete_owned(
                _db,
                std::format("DELETE FROM {} WHERE source_id=?1 AND owner=?2 AND name=?3;", ownedTable(kind)).c_str());
            sqlite3_bind_text(delete_owned.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            bindAddress(delete_owned.get(), 2, old_owner);
            sqlite3_bind_text(delete_owned.get(), 3, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(delete_owned.step() != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(_db));

            storage::sqlite::Statement insert_owned(
                _db,
                std::format("INSERT INTO {}(source_id,owner,name) VALUES(?1,?2,?3);", ownedTable(kind)).c_str());
            sqlite3_bind_text(insert_owned.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            bindAddress(insert_owned.get(), 2, new_owner);
            sqlite3_bind_text(insert_owned.get(), 3, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            if(insert_owned.step() != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(_db));

            if(!_commitTransaction()) throw std::runtime_error("commit failed");
            return true;
        }
        catch(...)
        {
            _rollbackTransaction();
            return false;
        }
    }

    std::vector<ArtifactReference> SQLiteRegistryStore::getArtifactsDue(
        const std::int64_t now_ms,
        const std::size_t limit) const
    {
        std::vector<ArtifactReference> references;
        try
        {
            storage::sqlite::Statement stmt(
                _db,
                "SELECT kind,source_id,name,content_hash,metadata_hash,runtime_code_hash,artifact_state,artifact_attempts FROM ("
                "SELECT 0 AS kind,source_id,name,content_hash,metadata_hash,runtime_code_hash,artifact_state,artifact_attempts,artifact_retry_at_ms,active FROM connectors "
                "UNION ALL SELECT 1,source_id,name,content_hash,metadata_hash,runtime_code_hash,artifact_state,artifact_attempts,artifact_retry_at_ms,active FROM transformations "
                "UNION ALL SELECT 2,source_id,name,content_hash,metadata_hash,runtime_code_hash,artifact_state,artifact_attempts,artifact_retry_at_ms,active FROM conditions) "
                "WHERE active=1 AND content_hash<>'' AND artifact_state<>'available' AND artifact_retry_at_ms<=?1 "
                "ORDER BY artifact_retry_at_ms,source_id,name LIMIT ?2;");
            sqlite3_bind_int64(stmt.get(), 1, now_ms);
            sqlite3_bind_int(stmt.get(), 2, toSqliteInt(limit));

            while(stmt.step() == SQLITE_ROW)
            {
                references.push_back(ArtifactReference{
                    .kind = static_cast<EntityKind>(sqlite3_column_int(stmt.get(), 0)),
                    .source_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1)),
                    .name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2)),
                    .content_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3)),
                    .metadata_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4)),
                    .runtime_code_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5)),
                    .state = parseArtifactState(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6))),
                    .attempts = static_cast<std::uint32_t>(std::clamp<sqlite3_int64>(
                        sqlite3_column_int64(stmt.get(), 7), 0, 8))});
            }
        }
        catch(const std::exception & error)
        {
            spdlog::error("SQLite artifact work query failed: {}", error.what());
        }
        return references;
    }

    bool SQLiteRegistryStore::setArtifactState(
        const EntityKind kind,
        const std::string & source_id,
        const std::string & name,
        const pt::ArtifactState state,
        const std::int64_t retry_at_ms)
    {
        try
        {
            storage::sqlite::Statement stmt(
                _db,
                std::format(
                    "UPDATE {} SET artifact_state=?1,artifact_attempts=CASE WHEN ?1='available' THEN 0 ELSE artifact_attempts+1 END,"
                    "artifact_retry_at_ms=?2 WHERE source_id=?3 AND name=?4 AND active=1;",
                    entityTable(kind)).c_str());
            const std::string_view state_name = pt::artifactStateName(state);
            sqlite3_bind_text(stmt.get(), 1, state_name.data(), static_cast<int>(state_name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt.get(), 2, retry_at_ms);
            sqlite3_bind_text(stmt.get(), 3, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 4, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            return stmt.step() == SQLITE_DONE && sqlite3_changes(_db) == 1;
        }
        catch(const std::exception & error)
        {
            spdlog::error("SQLite artifact state update failed for '{}': {}", name, error.what());
            return false;
        }
    }

    bool SQLiteRegistryStore::addConnectorVersion(
        const EntitySource & source,
        const chain::Address & address,
        const ConnectorRecord & record,
        const evmc::bytes32 & format_hash,
        const std::vector<ScalarLabel> & canonical_scalar_labels)
    {
        const std::string & connector_name = record.connector().name();
        const auto owner_opt = evmc::from_hex<chain::Address>(record.owner());
        if(!owner_opt)
        {
            spdlog::error("Failed to parse connector owner for `{}`", connector_name);
            return false;
        }

        spdlog::debug(
            "DB add connector name={} runtime_address={} format_hash={} scalar_labels={}",
            connector_name,
            evmc::hex(address),
            evmc::hex(format_hash),
            canonical_scalar_labels.size());

        if(!_beginTransaction())
        {
            return false;
        }

        auto rollback_with_log = [&](const char * reason)
        {
            spdlog::debug(
                "DB remove connector pending changes name={} runtime_address={} reason={}",
                connector_name,
                evmc::hex(address),
                reason);
            _rollbackTransaction();
        };

        try
        {
            if(!_insertConnectorRows(source, address, *owner_opt, format_hash, record, canonical_scalar_labels))
            {
                rollback_with_log("insert connector rows");
                return false;
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite addConnector failed: {}", e.what());
            rollback_with_log("exception");
            return false;
        }

        if(!_commitTransaction())
        {
            rollback_with_log("commit failed");
            return false;
        }

        spdlog::debug("DB add connector committed name={} runtime_address={}", connector_name, evmc::hex(address));
        return true;
    }

    bool SQLiteRegistryStore::addConnectorsBatch(const std::vector<ConnectorBatchItem> & items, bool all_or_nothing)
    {
        if(items.empty())
        {
            return true;
        }

        if(!_beginTransaction())
        {
            return false;
        }

        bool all_ok = true;
        std::size_t inserted_count = 0;
        std::size_t failed_count = 0;
        auto rollback_batch_with_log = [&](const char * reason)
        {
            spdlog::debug("DB remove connector batch pending changes count={} reason={}", items.size(), reason);
            _rollbackTransaction();
        };

        try
        {
            for(const ConnectorBatchItem & item : items)
            {
                const bool use_savepoint = !all_or_nothing;
                bool savepoint_active = false;
                if(use_savepoint)
                {
                    if(!_exec("SAVEPOINT connector_batch_item;"))
                    {
                        all_ok = false;
                        ++failed_count;
                        continue;
                    }
                    savepoint_active = true;
                }

                auto fail_item = [&](const char * reason)
                {
                    spdlog::debug("DB remove connector pending changes (batch item) name={} runtime_address={} reason={}", item.record.connector().name(), evmc::hex(item.address), reason);
                    if(savepoint_active)
                    {
                        (void)_exec("ROLLBACK TO SAVEPOINT connector_batch_item;");
                        (void)_exec("RELEASE SAVEPOINT connector_batch_item;");
                        savepoint_active = false;
                    }
                    return false;
                };

                const auto owner_opt = evmc::from_hex<chain::Address>(item.record.owner());
                if(!owner_opt)
                {
                    all_ok = false;
                    if(all_or_nothing)
                    {
                        rollback_batch_with_log("invalid owner");
                        return false;
                    }
                    (void)fail_item("invalid owner");
                    ++failed_count;
                    continue;
                }

                bool item_ok = true;

                if(!_insertConnectorRows(EntitySource{}, chain::Address{}, *owner_opt, item.format_hash, item.record, item.canonical_scalar_labels))
                {
                    item_ok = fail_item("insert connector rows");
                }

                if(item_ok && savepoint_active)
                {
                    if(!_exec("RELEASE SAVEPOINT connector_batch_item;"))
                    {
                        item_ok = fail_item("release savepoint");
                    }
                    else
                    {
                        savepoint_active = false;
                    }
                }

                if(!item_ok)
                {
                    all_ok = false;
                    ++failed_count;
                    if(all_or_nothing)
                    {
                        rollback_batch_with_log("batch item failure");
                        return false;
                    }
                }
                else
                {
                    ++inserted_count;
                }
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite addConnectorsBatch failed: {}", e.what());
            rollback_batch_with_log("exception");
            return false;
        }

        if(!_commitTransaction())
        {
            rollback_batch_with_log("commit failed");
            return false;
        }

        spdlog::debug("DB add connector batch committed inserted={} failed={}", inserted_count, failed_count);
        return all_ok;
    }

    std::size_t SQLiteRegistryStore::getFormatConnectorNamesCount(const evmc::bytes32 & format_hash) const
    {
        try
        {
            storage::sqlite::Statement stmt(_db, "SELECT COUNT(*) FROM format_members WHERE format_hash = ?1;");
            bindBytes32(stmt.get(), 1, format_hash);
            if(stmt.step() != SQLITE_ROW)
            {
                return 0;
            }
            return static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 0));
        }
        catch(const std::exception & e)
        {
            spdlog::error(
                "SQLite getFormatConnectorNamesCount query failed for format_hash={}: {}",
                evmc::hex(format_hash),
                e.what());
            return 0;
        }
    }

    NameCursorPage SQLiteRegistryStore::getFormatConnectorNamesCursor(
        const evmc::bytes32 & format_hash,
        const std::optional<NameCursor> & after,
        std::size_t limit) const
    {
        NameCursorPage page;
        if(limit == 0)
        {
            return page;
        }

        try
        {
            const std::size_t query_limit = limit + 1;
            if(after.has_value())
            {
                storage::sqlite::Statement stmt(_db, "SELECT name FROM format_members WHERE format_hash = ?1 AND name > ?2 ORDER BY name ASC LIMIT ?3;");
                bindBytes32(stmt.get(), 1, format_hash);
                sqlite3_bind_text(stmt.get(), 2, after->c_str(), static_cast<int>(after->size()), SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt.get(), 3, toSqliteInt(query_limit));
                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const unsigned char * name_text = sqlite3_column_text(stmt.get(), 0);
                    if(name_text != nullptr)
                    {
                        page.entries.emplace_back(reinterpret_cast<const char *>(name_text));
                    }
                }
            }
            else
            {
                storage::sqlite::Statement stmt(_db, "SELECT name FROM format_members WHERE format_hash = ?1 ORDER BY name ASC LIMIT ?2;");
                bindBytes32(stmt.get(), 1, format_hash);
                sqlite3_bind_int(stmt.get(), 2, toSqliteInt(query_limit));
                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const unsigned char * name_text = sqlite3_column_text(stmt.get(), 0);
                    if(name_text != nullptr)
                    {
                        page.entries.emplace_back(reinterpret_cast<const char *>(name_text));
                    }
                }
            }

            if(page.entries.size() > limit)
            {
                page.has_more = true;
                page.entries.resize(limit);
            }
            if(page.has_more && !page.entries.empty())
            {
                page.next_after = page.entries.back();
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error(
                "SQLite getFormatConnectorNamesCursor query failed for format_hash={} after={} limit={}: {}",
                evmc::hex(format_hash),
                after.value_or("<none>"),
                limit,
                e.what());
            return NameCursorPage{};
        }

        return page;
    }

    std::size_t SQLiteRegistryStore::getFormatsCount() const
    {
        try
        {
            storage::sqlite::Statement stmt(_db, "SELECT COUNT(DISTINCT format_hash) FROM format_members;");
            if(stmt.step() != SQLITE_ROW)
            {
                return 0;
            }
            return static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 0));
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite getFormatsCount query failed: {}", e.what());
            return 0;
        }
    }

    NameCursorPage SQLiteRegistryStore::getFormatsCursor(
        const std::optional<evmc::bytes32> & after,
        std::size_t limit) const
    {
        NameCursorPage page;
        if(limit == 0)
        {
            return page;
        }

        try
        {
            const std::size_t query_limit = limit + 1;
            if(after.has_value())
            {
                storage::sqlite::Statement stmt(
                    _db,
                    "SELECT DISTINCT format_hash FROM format_members "
                    "WHERE format_hash > ?1 ORDER BY format_hash ASC LIMIT ?2;");
                bindBytes32(stmt.get(), 1, *after);
                sqlite3_bind_int(stmt.get(), 2, toSqliteInt(query_limit));
                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const auto format_hash = columnBytes32(stmt.get(), 0);
                    if(format_hash.has_value())
                    {
                        page.entries.emplace_back(evmc::hex(*format_hash));
                    }
                }
            }
            else
            {
                storage::sqlite::Statement stmt(
                    _db,
                    "SELECT DISTINCT format_hash FROM format_members "
                    "ORDER BY format_hash ASC LIMIT ?1;");
                sqlite3_bind_int(stmt.get(), 1, toSqliteInt(query_limit));
                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const auto format_hash = columnBytes32(stmt.get(), 0);
                    if(format_hash.has_value())
                    {
                        page.entries.emplace_back(evmc::hex(*format_hash));
                    }
                }
            }

            if(page.entries.size() > limit)
            {
                page.has_more = true;
                page.entries.resize(limit);
            }
            if(page.has_more && !page.entries.empty())
            {
                page.next_after = page.entries.back();
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error(
                "SQLite getFormatsCursor query failed after={} limit={}: {}",
                after.has_value() ? evmc::hex(*after) : std::string("<none>"),
                limit,
                e.what());
            return NameCursorPage{};
        }

        return page;
    }

    std::optional<std::vector<ScalarLabel>> SQLiteRegistryStore::getScalarLabelsByFormatHash(const evmc::bytes32 & format_hash) const
    {
        try
        {
            storage::sqlite::Statement stmt(
                _db,
                "SELECT scalar, path_hash, tail_id FROM scalar_labels_by_format WHERE format_hash = ?1 ORDER BY path_hash ASC, scalar ASC, tail_id ASC;");
            bindBytes32(stmt.get(), 1, format_hash);

            std::vector<ScalarLabel> labels;
            for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
            {
                const unsigned char * scalar_text = sqlite3_column_text(stmt.get(), 0);
                const auto path_hash = columnBytes32(stmt.get(), 1);
                const sqlite3_int64 tail_id_raw = sqlite3_column_int64(stmt.get(), 2);
                if(scalar_text == nullptr || !path_hash.has_value())
                {
                    continue;
                }
                if(tail_id_raw < 0 || tail_id_raw > static_cast<sqlite3_int64>(std::numeric_limits<std::uint32_t>::max()))
                {
                    continue;
                }
                labels.push_back(ScalarLabel{.scalar = std::string(reinterpret_cast<const char *>(scalar_text)), .path_hash = *path_hash, .tail_id = static_cast<std::uint32_t>(tail_id_raw)});
            }

            if(labels.empty())
            {
                return std::nullopt;
            }
            return labels;
        }
        catch(const std::exception & e)
        {
            spdlog::error(
                "SQLite getScalarLabelsByFormatHash query failed for format_hash={}: {}",
                evmc::hex(format_hash),
                e.what());
            return std::nullopt;
        }
    }

    bool SQLiteRegistryStore::hasTransformation(const std::string & name) const
    {
        try
        {
            if(_db == nullptr)
            {
                spdlog::error("SQLite hasTransformation called with null DB handle for name={}", name);
                return false;
            }
            
            spdlog::debug("SQLite::hasTransformation('{}'): prepare (db={})", name, static_cast<const void *>(_db));
            
            storage::sqlite::Statement stmt(_db, "SELECT 1 FROM transformations WHERE name=?1 AND active=1 LIMIT 1;");
            const int bind_rc = sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            
            spdlog::debug("SQLite::hasTransformation('{}'): bind rc={}", name, bind_rc);

            if(bind_rc != SQLITE_OK)
            {
                spdlog::error("SQLite::hasTransformation('{}'): bind failed rc={} err={}", name, bind_rc, sqlite3_errmsg(_db));
                return false;
            }
            const int rc = stmt.step();
            
            spdlog::debug("SQLite::hasTransformation('{}'): step rc={}", name, rc);
            
            return rc == SQLITE_ROW;
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite hasTransformation query failed for name={}: {}", name, e.what());
            return false;
        }
    }

    std::optional<TransformationRecordHandle> SQLiteRegistryStore::getTransformationRecordHandle(const std::string & name) const
    {
        try
        {
            
            spdlog::debug("SQLite::getTransformationRecordHandle('{}'): prepare", name);
            
            storage::sqlite::Statement stmt(
                _db,
                "SELECT owner, args_count FROM transformations WHERE name=?1 AND active=1 "
                "ORDER BY CASE WHEN source_id='local' THEN 1 ELSE 0 END, source_id LIMIT 1;");
            sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);

            spdlog::debug("SQLite::getTransformationRecordHandle('{}'): bound name", name);

            const int rc = stmt.step();

            spdlog::debug("SQLite::getTransformationRecordHandle('{}'): step rc={}", name, rc);

            if(rc != SQLITE_ROW)
            {
                return std::nullopt;
            }

            const auto owner = columnAddress(stmt.get(), 0);
            if(!owner)
            {
                spdlog::error("SQLite transformation `{}` has invalid owner column", name);
                return std::nullopt;
            }

            TransformationRecord record;
            Transformation * transformation = record.mutable_transformation();
            transformation->set_name(name);
            transformation->set_args_count(static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 1)));
            record.set_owner(evmc::hex(*owner));

            spdlog::debug("SQLite::getTransformationRecordHandle('{}'): reassembled record", name);
            return std::make_shared<TransformationRecord>(std::move(record));
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite getTransformationRecordHandle query failed for name={}: {}", name, e.what());
            return std::nullopt;
        }
    }

    bool SQLiteRegistryStore::addTransformation(const chain::Address & address, const TransformationRecord & record)
    {
        return addTransformationVersion(EntitySource{}, address, record);
    }

    bool SQLiteRegistryStore::addTransformationVersion(
        const EntitySource & source,
        const chain::Address & address,
        const TransformationRecord & record)
    {
        const std::string & transformation_name = record.transformation().name();
        const auto owner_opt = evmc::from_hex<chain::Address>(record.owner());
        if(!owner_opt)
        {
            spdlog::error("Failed to parse transformation owner for `{}`", transformation_name);
            return false;
        }

        spdlog::debug(
            "DB add transformation name={} runtime_address={}",
            transformation_name,
            evmc::hex(address));

        if(!_beginTransaction())
        {
            return false;
        }

        auto rollback_with_log = [&](const char * reason)
        {
            spdlog::debug("DB remove transformation pending changes name={} runtime_address={} reason={}", transformation_name, evmc::hex(address), reason);
            _rollbackTransaction();
        };

        try
        {
            storage::sqlite::Statement insert_entity(
                _db,
                "INSERT INTO transformations(source_id,chain_namespace,chain_id,registry_address,entity_address,name,owner,args_count,content_hash,metadata_hash,runtime_code_hash,artifact_state,definition_conflict,origin_block_hash,origin_log_index) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15) "
                "ON CONFLICT(source_id,name) DO UPDATE SET "
                "chain_namespace=excluded.chain_namespace, chain_id=excluded.chain_id, "
                "registry_address=excluded.registry_address, entity_address=excluded.entity_address, "
                "owner=excluded.owner, args_count=excluded.args_count, "
                "content_hash=excluded.content_hash, metadata_hash=excluded.metadata_hash, runtime_code_hash=excluded.runtime_code_hash, artifact_state=excluded.artifact_state, definition_conflict=excluded.definition_conflict, "
                "artifact_attempts=0, artifact_retry_at_ms=0, "
                "origin_block_hash=excluded.origin_block_hash, origin_log_index=excluded.origin_log_index, "
                "active=1;");
            sqlite3_bind_text(insert_entity.get(), 1, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_entity.get(), 2, source.chain_namespace.c_str(), static_cast<int>(source.chain_namespace.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_entity.get(), 3, static_cast<sqlite3_int64>(source.chain_id));
            sqlite3_bind_text(insert_entity.get(), 4, source.registry_address.c_str(), static_cast<int>(source.registry_address.size()), SQLITE_TRANSIENT);
            bindAddress(insert_entity.get(), 5, address);
            sqlite3_bind_text(insert_entity.get(), 6, transformation_name.c_str(), static_cast<int>(transformation_name.size()), SQLITE_TRANSIENT);
            bindAddress(insert_entity.get(), 7, *owner_opt);
            sqlite3_bind_int64(insert_entity.get(), 8, static_cast<sqlite3_int64>(record.transformation().args_count()));
            sqlite3_bind_text(insert_entity.get(), 9, source.content_hash.c_str(), static_cast<int>(source.content_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_entity.get(), 10, source.metadata_hash.c_str(), static_cast<int>(source.metadata_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_entity.get(), 11, source.runtime_code_hash.c_str(), static_cast<int>(source.runtime_code_hash.size()), SQLITE_TRANSIENT);
            const std::string_view artifact_state = pt::artifactStateName(source.artifact_state);
            sqlite3_bind_text(insert_entity.get(), 12, artifact_state.data(), static_cast<int>(artifact_state.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_entity.get(), 13, source.definition_conflict ? 1 : 0);
            sqlite3_bind_text(insert_entity.get(), 14, source.block_hash.c_str(), static_cast<int>(source.block_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_entity.get(), 15, source.log_index);
            if(insert_entity.step() != SQLITE_DONE)
            {
                rollback_with_log("insert transformations");
                return false;
            }

            // A re-projected version may carry another owner; its old index row goes first.
            storage::sqlite::Statement delete_owned(_db, "DELETE FROM owned_transformations WHERE source_id=?1 AND name=?2;");
            sqlite3_bind_text(delete_owned.get(), 1, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(delete_owned.get(), 2, transformation_name.c_str(), static_cast<int>(transformation_name.size()), SQLITE_TRANSIENT);
            if(delete_owned.step() != SQLITE_DONE)
            {
                rollback_with_log("delete owned_transformations");
                return false;
            }

            storage::sqlite::Statement insert_owned(_db, "INSERT OR REPLACE INTO owned_transformations(owner, name, source_id) VALUES(?1, ?2, ?3);");
            bindAddress(insert_owned.get(), 1, *owner_opt);
            sqlite3_bind_text(insert_owned.get(), 2, transformation_name.c_str(), static_cast<int>(transformation_name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_owned.get(), 3, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            if(insert_owned.step() != SQLITE_DONE)
            {
                rollback_with_log("insert owned_transformations");
                return false;
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite addTransformation failed: {}", e.what());
            rollback_with_log("exception");
            return false;
        }

        if(!_commitTransaction())
        {
            rollback_with_log("commit failed");
            return false;
        }

        spdlog::debug("DB add transformation committed name={} runtime_address={}", transformation_name, evmc::hex(address));
        return true;
    }

    bool SQLiteRegistryStore::addTransformationsBatch(const std::vector<TransformationBatchItem> & items, bool all_or_nothing)
    {
        if(items.empty())
        {
            return true;
        }

        if(!_beginTransaction())
        {
            return false;
        }

        bool all_ok = true;
        std::size_t inserted_count = 0;
        std::size_t failed_count = 0;
        auto rollback_batch_with_log = [&](const char * reason)
        {
            spdlog::debug("DB remove transformation batch pending changes count={} reason={}", items.size(), reason);
            _rollbackTransaction();
        };

        try
        {
            storage::sqlite::Statement insert_entity(_db, "INSERT INTO transformations(name, owner, args_count) VALUES(?1, ?2, ?3);");
            storage::sqlite::Statement insert_owned(_db, "INSERT OR REPLACE INTO owned_transformations(owner, name) VALUES(?1, ?2);");
            for(const TransformationBatchItem & item : items)
            {
                const bool use_savepoint = !all_or_nothing;
                bool savepoint_active = false;
                if(use_savepoint)
                {
                    if(!_exec("SAVEPOINT transformation_batch_item;"))
                    {
                        all_ok = false;
                        ++failed_count;
                        continue;
                    }
                    savepoint_active = true;
                }

                auto fail_item = [&](const char * reason)
                {
                    spdlog::debug(
                        "DB remove transformation pending changes (batch item) name={} runtime_address={} reason={}",
                        item.record.transformation().name(),
                        evmc::hex(item.address),
                        reason);
                    if(savepoint_active)
                    {
                        (void)_exec("ROLLBACK TO SAVEPOINT transformation_batch_item;");
                        (void)_exec("RELEASE SAVEPOINT transformation_batch_item;");
                        savepoint_active = false;
                    }
                    return false;
                };

                const auto owner_opt = evmc::from_hex<chain::Address>(item.record.owner());
                if(!owner_opt)
                {
                    all_ok = false;
                    if(all_or_nothing)
                    {
                        rollback_batch_with_log("invalid owner");
                        return false;
                    }
                    (void)fail_item("invalid owner");
                    ++failed_count;
                    continue;
                }

                bool item_ok = true;

                insert_entity.reset();
                sqlite3_bind_text(insert_entity.get(), 1, item.record.transformation().name().c_str(), static_cast<int>(item.record.transformation().name().size()), SQLITE_TRANSIENT);
                bindAddress(insert_entity.get(), 2, *owner_opt);
                sqlite3_bind_int64(insert_entity.get(), 3, static_cast<sqlite3_int64>(item.record.transformation().args_count()));
                if(insert_entity.step() != SQLITE_DONE)
                {
                    item_ok = fail_item("insert transformations");
                }

                if(item_ok)
                {
                    insert_owned.reset();
                    bindAddress(insert_owned.get(), 1, *owner_opt);
                    sqlite3_bind_text(insert_owned.get(), 2, item.record.transformation().name().c_str(), static_cast<int>(item.record.transformation().name().size()), SQLITE_TRANSIENT);
                    if(insert_owned.step() != SQLITE_DONE)
                    {
                        item_ok = fail_item("insert owned_transformations");
                    }
                }

                if(item_ok && savepoint_active)
                {
                    if(!_exec("RELEASE SAVEPOINT transformation_batch_item;"))
                    {
                        item_ok = fail_item("release savepoint");
                    }
                    else
                    {
                        savepoint_active = false;
                    }
                }

                if(!item_ok)
                {
                    all_ok = false;
                    ++failed_count;
                    if(all_or_nothing)
                    {
                        rollback_batch_with_log("batch item failure");
                        return false;
                    }
                }
                else
                {
                    ++inserted_count;
                }
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite addTransformationsBatch failed: {}", e.what());
            rollback_batch_with_log("exception");
            return false;
        }

        if(!_commitTransaction())
        {
            rollback_batch_with_log("commit failed");
            return false;
        }

        spdlog::debug("DB add transformation batch committed inserted={} failed={}", inserted_count, failed_count);
        return all_ok;
    }

    bool SQLiteRegistryStore::hasCondition(const std::string & name) const
    {
        try
        {
            if(_db == nullptr)
            {
                spdlog::error("SQLite hasCondition called with null DB handle for name={}", name);
                return false;
            }
            
            spdlog::debug("SQLite::hasCondition('{}'): prepare (db={})", name, static_cast<const void *>(_db));
            
            storage::sqlite::Statement stmt(_db, "SELECT 1 FROM conditions WHERE name=?1 AND active=1 LIMIT 1;");
            const int bind_rc = sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
            
            spdlog::debug("SQLite::hasCondition('{}'): bind rc={}", name, bind_rc);
            
            if(bind_rc != SQLITE_OK)
            {
                spdlog::error("SQLite::hasCondition('{}'): bind failed rc={} err={}", name, bind_rc, sqlite3_errmsg(_db));
                return false;
            }
            const int rc = stmt.step();
            
            spdlog::debug("SQLite::hasCondition('{}'): step rc={}", name, rc);
            
            return rc == SQLITE_ROW;
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite hasCondition query failed for name={}: {}", name, e.what());
            return false;
        }
    }

    std::optional<ConditionRecordHandle> SQLiteRegistryStore::getConditionRecordHandle(const std::string & name) const
    {
        try
        {
            
            spdlog::debug("SQLite::getConditionRecordHandle('{}'): prepare", name);
            
            storage::sqlite::Statement stmt(
                _db,
                "SELECT owner, args_count FROM conditions WHERE name=?1 AND active=1 "
                "ORDER BY CASE WHEN source_id='local' THEN 1 ELSE 0 END, source_id LIMIT 1;");
            sqlite3_bind_text(stmt.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);

            spdlog::debug("SQLite::getConditionRecordHandle('{}'): bound name", name);

            const int rc = stmt.step();

            spdlog::debug("SQLite::getConditionRecordHandle('{}'): step rc={}", name, rc);

            if(rc != SQLITE_ROW)
            {
                return std::nullopt;
            }

            const auto owner = columnAddress(stmt.get(), 0);
            if(!owner)
            {
                spdlog::error("SQLite condition `{}` has invalid owner column", name);
                return std::nullopt;
            }

            ConditionRecord record;
            Condition * condition = record.mutable_condition();
            condition->set_name(name);
            condition->set_args_count(static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 1)));
            record.set_owner(evmc::hex(*owner));

            spdlog::debug("SQLite::getConditionRecordHandle('{}'): reassembled record", name);
            return std::make_shared<ConditionRecord>(std::move(record));
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite getConditionRecordHandle query failed for name={}: {}", name, e.what());
            return std::nullopt;
        }
    }

    bool SQLiteRegistryStore::addCondition(const chain::Address & address, const ConditionRecord & record)
    {
        return addConditionVersion(EntitySource{}, address, record);
    }

    bool SQLiteRegistryStore::addConditionVersion(
        const EntitySource & source,
        const chain::Address & address,
        const ConditionRecord & record)
    {
        const std::string & condition_name = record.condition().name();
        const auto owner_opt = evmc::from_hex<chain::Address>(record.owner());
        if(!owner_opt)
        {
            spdlog::error("Failed to parse condition owner for `{}`", condition_name);
            return false;
        }

        spdlog::debug(
            "DB add condition name={} runtime_address={}",
            condition_name,
            evmc::hex(address));

        if(!_beginTransaction())
        {
            return false;
        }

        auto rollback_with_log = [&](const char * reason)
        {
            spdlog::debug("DB remove condition pending changes name={} runtime_address={} reason={}", condition_name, evmc::hex(address), reason);
            _rollbackTransaction();
        };

        try
        {
            storage::sqlite::Statement insert_entity(
                _db,
                "INSERT INTO conditions(source_id,chain_namespace,chain_id,registry_address,entity_address,name,owner,args_count,content_hash,metadata_hash,runtime_code_hash,artifact_state,definition_conflict,origin_block_hash,origin_log_index) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15) "
                "ON CONFLICT(source_id,name) DO UPDATE SET "
                "chain_namespace=excluded.chain_namespace, chain_id=excluded.chain_id, "
                "registry_address=excluded.registry_address, entity_address=excluded.entity_address, "
                "owner=excluded.owner, args_count=excluded.args_count, "
                "content_hash=excluded.content_hash, metadata_hash=excluded.metadata_hash, runtime_code_hash=excluded.runtime_code_hash, artifact_state=excluded.artifact_state, definition_conflict=excluded.definition_conflict, "
                "artifact_attempts=0, artifact_retry_at_ms=0, "
                "origin_block_hash=excluded.origin_block_hash, origin_log_index=excluded.origin_log_index, "
                "active=1;");
            sqlite3_bind_text(insert_entity.get(), 1, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_entity.get(), 2, source.chain_namespace.c_str(), static_cast<int>(source.chain_namespace.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_entity.get(), 3, static_cast<sqlite3_int64>(source.chain_id));
            sqlite3_bind_text(insert_entity.get(), 4, source.registry_address.c_str(), static_cast<int>(source.registry_address.size()), SQLITE_TRANSIENT);
            bindAddress(insert_entity.get(), 5, address);
            sqlite3_bind_text(insert_entity.get(), 6, condition_name.c_str(), static_cast<int>(condition_name.size()), SQLITE_TRANSIENT);
            bindAddress(insert_entity.get(), 7, *owner_opt);
            sqlite3_bind_int64(insert_entity.get(), 8, static_cast<sqlite3_int64>(record.condition().args_count()));
            sqlite3_bind_text(insert_entity.get(), 9, source.content_hash.c_str(), static_cast<int>(source.content_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_entity.get(), 10, source.metadata_hash.c_str(), static_cast<int>(source.metadata_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_entity.get(), 11, source.runtime_code_hash.c_str(), static_cast<int>(source.runtime_code_hash.size()), SQLITE_TRANSIENT);
            const std::string_view artifact_state = pt::artifactStateName(source.artifact_state);
            sqlite3_bind_text(insert_entity.get(), 12, artifact_state.data(), static_cast<int>(artifact_state.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_entity.get(), 13, source.definition_conflict ? 1 : 0);
            sqlite3_bind_text(insert_entity.get(), 14, source.block_hash.c_str(), static_cast<int>(source.block_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_entity.get(), 15, source.log_index);
            if(insert_entity.step() != SQLITE_DONE)
            {
                rollback_with_log("insert conditions");
                return false;
            }

            // A re-projected version may carry another owner; its old index row goes first.
            storage::sqlite::Statement delete_owned(_db, "DELETE FROM owned_conditions WHERE source_id=?1 AND name=?2;");
            sqlite3_bind_text(delete_owned.get(), 1, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(delete_owned.get(), 2, condition_name.c_str(), static_cast<int>(condition_name.size()), SQLITE_TRANSIENT);
            if(delete_owned.step() != SQLITE_DONE)
            {
                rollback_with_log("delete owned_conditions");
                return false;
            }

            storage::sqlite::Statement insert_owned(_db, "INSERT OR REPLACE INTO owned_conditions(owner, name, source_id) VALUES(?1, ?2, ?3);");
            bindAddress(insert_owned.get(), 1, *owner_opt);
            sqlite3_bind_text(insert_owned.get(), 2, condition_name.c_str(), static_cast<int>(condition_name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_owned.get(), 3, source.source_id.c_str(), static_cast<int>(source.source_id.size()), SQLITE_TRANSIENT);
            if(insert_owned.step() != SQLITE_DONE)
            {
                rollback_with_log("insert owned_conditions");
                return false;
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite addCondition failed: {}", e.what());
            rollback_with_log("exception");
            return false;
        }

        if(!_commitTransaction())
        {
            rollback_with_log("commit failed");
            return false;
        }

        spdlog::debug("DB add condition committed name={} runtime_address={}", condition_name, evmc::hex(address));
        return true;
    }

    bool SQLiteRegistryStore::addConditionsBatch(const std::vector<ConditionBatchItem> & items, bool all_or_nothing)
    {
        if(items.empty())
        {
            return true;
        }

        if(!_beginTransaction())
        {
            return false;
        }

        bool all_ok = true;
        std::size_t inserted_count = 0;
        std::size_t failed_count = 0;
        auto rollback_batch_with_log = [&](const char * reason)
        {
            spdlog::debug("DB remove condition batch pending changes count={} reason={}", items.size(), reason);
            _rollbackTransaction();
        };

        try
        {
            storage::sqlite::Statement insert_entity(_db, "INSERT INTO conditions(name, owner, args_count) VALUES(?1, ?2, ?3);");
            storage::sqlite::Statement insert_owned(_db, "INSERT OR REPLACE INTO owned_conditions(owner, name) VALUES(?1, ?2);");
            for(const ConditionBatchItem & item : items)
            {
                const bool use_savepoint = !all_or_nothing;
                bool savepoint_active = false;
                if(use_savepoint)
                {
                    if(!_exec("SAVEPOINT condition_batch_item;"))
                    {
                        all_ok = false;
                        ++failed_count;
                        continue;
                    }
                    savepoint_active = true;
                }

                auto fail_item = [&](const char * reason)
                {
                    spdlog::debug(
                        "DB remove condition pending changes (batch item) name={} runtime_address={} reason={}",
                        item.record.condition().name(),
                        evmc::hex(item.address),
                        reason);
                    if(savepoint_active)
                    {
                        (void)_exec("ROLLBACK TO SAVEPOINT condition_batch_item;");
                        (void)_exec("RELEASE SAVEPOINT condition_batch_item;");
                        savepoint_active = false;
                    }
                    return false;
                };

                const auto owner_opt = evmc::from_hex<chain::Address>(item.record.owner());
                if(!owner_opt)
                {
                    all_ok = false;
                    if(all_or_nothing)
                    {
                        rollback_batch_with_log("invalid owner");
                        return false;
                    }
                    (void)fail_item("invalid owner");
                    ++failed_count;
                    continue;
                }

                bool item_ok = true;

                insert_entity.reset();
                sqlite3_bind_text(insert_entity.get(), 1, item.record.condition().name().c_str(), static_cast<int>(item.record.condition().name().size()), SQLITE_TRANSIENT);
                bindAddress(insert_entity.get(), 2, *owner_opt);
                sqlite3_bind_int64(insert_entity.get(), 3, static_cast<sqlite3_int64>(item.record.condition().args_count()));
                if(insert_entity.step() != SQLITE_DONE)
                {
                    item_ok = fail_item("insert conditions");
                }

                if(item_ok)
                {
                    insert_owned.reset();
                    bindAddress(insert_owned.get(), 1, *owner_opt);
                    sqlite3_bind_text(insert_owned.get(), 2, item.record.condition().name().c_str(), static_cast<int>(item.record.condition().name().size()), SQLITE_TRANSIENT);
                    if(insert_owned.step() != SQLITE_DONE)
                    {
                        item_ok = fail_item("insert owned_conditions");
                    }
                }

                if(item_ok && savepoint_active)
                {
                    if(!_exec("RELEASE SAVEPOINT condition_batch_item;"))
                    {
                        item_ok = fail_item("release savepoint");
                    }
                    else
                    {
                        savepoint_active = false;
                    }
                }

                if(!item_ok)
                {
                    all_ok = false;
                    ++failed_count;
                    if(all_or_nothing)
                    {
                        rollback_batch_with_log("batch item failure");
                        return false;
                    }
                }
                else
                {
                    ++inserted_count;
                }
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite addConditionsBatch failed: {}", e.what());
            rollback_batch_with_log("exception");
            return false;
        }

        if(!_commitTransaction())
        {
            rollback_batch_with_log("commit failed");
            return false;
        }

        spdlog::debug("DB add condition batch committed inserted={} failed={}", inserted_count, failed_count);
        return all_ok;
    }

    NameCursorPage SQLiteRegistryStore::_getOwnedCursorFromTable(
        const char * table_name,
        const chain::Address & owner,
        const std::optional<NameCursor> & after,
        std::size_t limit) const
    {
        NameCursorPage page;
        if(limit == 0)
        {
            return page;
        }

        try
        {
            const std::size_t query_limit = limit + 1;
            if(after.has_value())
            {
                const std::string sql =
                    std::string("SELECT name FROM ") + table_name +
                    " WHERE owner = ?1 AND name > ?2 ORDER BY name ASC LIMIT ?3;";
                storage::sqlite::Statement stmt(_db, sql.c_str());
                bindAddress(stmt.get(), 1, owner);
                sqlite3_bind_text(stmt.get(), 2, after->c_str(), static_cast<int>(after->size()), SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt.get(), 3, toSqliteInt(query_limit));

                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const unsigned char * name_text = sqlite3_column_text(stmt.get(), 0);
                    if(name_text != nullptr)
                    {
                        page.entries.push_back(std::string(reinterpret_cast<const char *>(name_text)));
                    }
                }
            }
            else
            {
                const std::string sql =
                    std::string("SELECT name FROM ") + table_name +
                    " WHERE owner = ?1 ORDER BY name ASC LIMIT ?2;";
                storage::sqlite::Statement stmt(_db, sql.c_str());
                bindAddress(stmt.get(), 1, owner);
                sqlite3_bind_int(stmt.get(), 2, toSqliteInt(query_limit));
                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const unsigned char * name_text = sqlite3_column_text(stmt.get(), 0);
                    if(name_text != nullptr)
                    {
                        page.entries.push_back(std::string(reinterpret_cast<const char *>(name_text)));
                    }
                }
            }

            if(page.entries.size() > limit)
            {
                page.has_more = true;
                page.entries.resize(limit);
            }

            if(page.has_more && !page.entries.empty())
            {
                page.next_after = page.entries.back();
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error(
                "SQLite owned cursor query failed for table={} owner={} after={} limit={}: {}",
                table_name,
                evmc::hex(owner),
                after.value_or("<none>"),
                limit,
                e.what());
            return NameCursorPage{};
        }

        return page;
    }

    NameCursorPage SQLiteRegistryStore::getOwnedConnectorsCursor(const chain::Address & owner, const std::optional<NameCursor> & after, std::size_t limit) const
    {
        return _getOwnedCursorFromTable("owned_connectors", owner, after, limit);
    }

    NameCursorPage SQLiteRegistryStore::getOwnedTransformationsCursor(const chain::Address & owner, const std::optional<NameCursor> & after, std::size_t limit) const
    {
        return _getOwnedCursorFromTable("owned_transformations", owner, after, limit);
    }

    NameCursorPage SQLiteRegistryStore::getOwnedConditionsCursor(const chain::Address & owner, const std::optional<NameCursor> & after, std::size_t limit) const
    {
        return _getOwnedCursorFromTable("owned_conditions", owner, after, limit);
    }

    std::size_t SQLiteRegistryStore::getAccountsCount() const
    {
        try
        {
            storage::sqlite::Statement stmt(
                _db,
                "SELECT COUNT(*) FROM ("
                "SELECT owner FROM owned_connectors "
                "UNION "
                "SELECT owner FROM owned_transformations "
                "UNION "
                "SELECT owner FROM owned_conditions"
                ") AS all_owners;");
            if(stmt.step() != SQLITE_ROW)
            {
                return 0;
            }
            return static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 0));
        }
        catch(const std::exception & e)
        {
            spdlog::error("SQLite getAccountsCount query failed: {}", e.what());
            return 0;
        }
    }

    NameCursorPage SQLiteRegistryStore::getAccountsCursor(
        const std::optional<chain::Address> & after,
        std::size_t limit) const
    {
        NameCursorPage page;
        if(limit == 0)
        {
            return page;
        }

        try
        {
            const std::size_t query_limit = limit + 1;
            if(after.has_value())
            {
                storage::sqlite::Statement stmt(
                    _db,
                    "SELECT owner FROM ("
                    "SELECT owner FROM owned_connectors "
                    "UNION "
                    "SELECT owner FROM owned_transformations "
                    "UNION "
                    "SELECT owner FROM owned_conditions"
                    ") AS all_owners "
                    "WHERE owner > ?1 ORDER BY owner ASC LIMIT ?2;");
                bindAddress(stmt.get(), 1, *after);
                sqlite3_bind_int(stmt.get(), 2, toSqliteInt(query_limit));
                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const auto owner = columnAddress(stmt.get(), 0);
                    if(owner.has_value())
                    {
                        page.entries.emplace_back(evmc::hex(*owner));
                    }
                }
            }
            else
            {
                storage::sqlite::Statement stmt(
                    _db,
                    "SELECT owner FROM ("
                    "SELECT owner FROM owned_connectors "
                    "UNION "
                    "SELECT owner FROM owned_transformations "
                    "UNION "
                    "SELECT owner FROM owned_conditions"
                    ") AS all_owners "
                    "ORDER BY owner ASC LIMIT ?1;");
                sqlite3_bind_int(stmt.get(), 1, toSqliteInt(query_limit));
                for(int rc = stmt.step(); rc == SQLITE_ROW; rc = stmt.step())
                {
                    const auto owner = columnAddress(stmt.get(), 0);
                    if(owner.has_value())
                    {
                        page.entries.emplace_back(evmc::hex(*owner));
                    }
                }
            }

            if(page.entries.size() > limit)
            {
                page.has_more = true;
                page.entries.resize(limit);
            }
            if(page.has_more && !page.entries.empty())
            {
                page.next_after = page.entries.back();
            }
        }
        catch(const std::exception & e)
        {
            spdlog::error(
                "SQLite getAccountsCursor query failed after={} limit={}: {}",
                after.has_value() ? evmc::hex(*after) : std::string("<none>"),
                limit,
                e.what());
            return NameCursorPage{};
        }

        return page;
    }

    bool SQLiteRegistryStore::checkpointWal(const storage::sqlite::WalCheckpointMode mode) const
    {
        if(_db == nullptr)
        {
            return false;
        }

        const int set_short_timeout_res = sqlite3_busy_timeout(_db, SQLITE_CHECKPOINT_BUSY_TIMEOUT_MS);
        if(set_short_timeout_res != SQLITE_OK)
        {
            spdlog::warn(
                "Failed to set SQLite checkpoint busy timeout mode={} rc={} err={}",
                checkpointModeToString(mode),
                set_short_timeout_res,
                sqlite3_errmsg(_db));
        }

        int wal_frames_total = 0;
        int wal_frames_checkpointed = 0;
        const int res = sqlite3_wal_checkpoint_v2(
            _db,
            nullptr,
            storage::sqlite::toSqliteCheckpointMode(mode),
            &wal_frames_total,
            &wal_frames_checkpointed);
        (void)sqlite3_busy_timeout(_db, SQLITE_DEFAULT_BUSY_TIMEOUT_MS);

        if(res != SQLITE_OK)
        {
            spdlog::warn(
                "SQLite WAL checkpoint failed mode={} rc={} err={}",
                storage::sqlite::checkpointModeToString(mode),
                res,
                sqlite3_errmsg(_db));
            return false;
        }

        spdlog::debug(
            "SQLite WAL checkpoint mode={} frames_total={} frames_checkpointed={}",
            storage::sqlite::checkpointModeToString(mode),
            wal_frames_total,
            wal_frames_checkpointed);
        return true;
    }
}


namespace dcn::parse
{
    Result<registry::NameCursor> parseNameCursor(const std::string & name_token)
    {
        if(name_token.empty())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Name cannot be empty"});
        }

        const auto first_non_space = std::find_if_not(
            name_token.begin(),
            name_token.end(),
            [](unsigned char ch)
            {
                return std::isspace(ch) != 0;
            });
        
        if(first_non_space == name_token.end())
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Name cannot be empty"});
        }

        const auto last_non_space = std::find_if_not(
            name_token.rbegin(),
            name_token.rend(),
            [](unsigned char ch)
            {
                return std::isspace(ch) != 0;
            }).base();
        
        return std::string(first_non_space, last_non_space);
    }

    Result<evmc::bytes32> parseFormatCursorHex(const std::string & cursor_token)
    {
        auto normalized_res = parseNameCursor(cursor_token);
        if(!normalized_res)
        {
            return std::unexpected(ParseError{
                ParseError::Kind::INVALID_VALUE,
                "Format cursor cannot be empty"});
        }

        std::string normalized = std::move(normalized_res.value());
        if(normalized.size() >= 2 && normalized.at(0) == '0' &&
           (normalized.at(1) == 'x' || normalized.at(1) == 'X'))
        {
            normalized.erase(0, 2);
        }

        if(normalized.size() != 64)
        {
            return std::unexpected(ParseError{
                ParseError::Kind::INVALID_VALUE,
                "Format cursor must be 32-byte hex"});
        }

        for(char & ch : normalized)
        {
            if(std::isxdigit(static_cast<unsigned char>(ch)) == 0)
            {
                return std::unexpected(ParseError{
                    ParseError::Kind::INVALID_VALUE,
                    "Format cursor must be hex"});
            }
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        const auto cursor = evmc::from_hex<evmc::bytes32>(normalized);
        if(!cursor.has_value())
        {
            return std::unexpected(ParseError{
                ParseError::Kind::INVALID_VALUE,
                "Format cursor parse failed"});
        }

        return *cursor;
    }
}
