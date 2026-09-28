#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "sqlite/exec.hpp"

namespace dcn::storage::sqlite
{
    bool exec(sqlite3 * db, const char * sql)
    {
        char * err = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if(rc == SQLITE_OK)
        {
            return true;
        }

        const std::string err_msg = (err == nullptr) ? std::string(sqlite3_errmsg(db)) : std::string(err);
        if(err != nullptr)
        {
            sqlite3_free(err);
        }

        spdlog::error("SQLite exec failed: {} | sql={}", err_msg, sql);
        return false;
    }

    namespace
    {
        int _queryInt(sqlite3 * db, const char * sql)
        {
            sqlite3_stmt * stmt = nullptr;
            if(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            {
                return -1;
            }
            const int value = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
            sqlite3_finalize(stmt);
            return value;
        }
    }

    bool ensureSchemaVersion(sqlite3 * db, const int version, const std::string_view store_name)
    {
        const int current = _queryInt(db, "PRAGMA user_version;");
        if(current == version)
        {
            return true;
        }

        if(current == 0 && _queryInt(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table';") == 0)
        {
            return exec(db, std::format("PRAGMA user_version={};", version).c_str());
        }

        spdlog::error(
            "{} database has schema version {}, this server needs version {}; "
            "schemas are not migrated, so move the database aside to let the server rebuild it",
            store_name,
            current,
            version);
        return false;
    }
}
