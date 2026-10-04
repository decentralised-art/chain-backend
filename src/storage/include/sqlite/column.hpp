#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <sqlite3.h>

namespace dcn::storage::sqlite
{
    // NULL columns read as std::nullopt.
    std::optional<std::int64_t> columnInt64Optional(sqlite3_stmt * stmt, int index);

    std::optional<std::string> columnTextOptional(sqlite3_stmt * stmt, int index);

    // std::nullopt binds as NULL.
    int bindOptionalInt64(sqlite3_stmt * stmt, int index, const std::optional<std::int64_t> & value);

    int bindOptionalText(sqlite3_stmt * stmt, int index, const std::optional<std::string> & value);
}
