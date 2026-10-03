#include "sqlite/column.hpp"

namespace dcn::storage::sqlite
{
    std::optional<std::int64_t> columnInt64Optional(sqlite3_stmt * stmt, const int index)
    {
        if(sqlite3_column_type(stmt, index) == SQLITE_NULL)
        {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt, index));
    }

    std::optional<std::string> columnTextOptional(sqlite3_stmt * stmt, const int index)
    {
        const unsigned char * txt = sqlite3_column_text(stmt, index);
        if(txt == nullptr)
        {
            return std::nullopt;
        }
        return std::string(reinterpret_cast<const char *>(txt));
    }

    int bindOptionalInt64(sqlite3_stmt * stmt, const int index, const std::optional<std::int64_t> & value)
    {
        if(!value.has_value())
        {
            return sqlite3_bind_null(stmt, index);
        }
        return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(*value));
    }

    int bindOptionalText(sqlite3_stmt * stmt, const int index, const std::optional<std::string> & value)
    {
        if(!value.has_value())
        {
            return sqlite3_bind_null(stmt, index);
        }
        return sqlite3_bind_text(stmt, index, value->c_str(), static_cast<int>(value->size()), SQLITE_TRANSIENT);
    }
}
