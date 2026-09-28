#pragma once

#include <string_view>

#include <sqlite3.h>

namespace dcn::storage::sqlite
{
    bool exec(sqlite3 * db, const char * sql);

    /// Stamps a database that has no tables yet with `version` (PRAGMA user_version) and
    /// refuses any other version, including an unstamped database that already has tables.
    /// Schemas are not migrated: a database of another version is moved aside and rebuilt.
    /// Must run before the store creates its tables.
    bool ensureSchemaVersion(sqlite3 * db, int version, std::string_view store_name);
}
