#include <sqlite3.h>

#include "sqlite/wal.hpp"

namespace dcn::storage::sqlite
{
    std::string checkpointModeToString(const storage::sqlite::WalCheckpointMode mode)
    {
        switch(mode)
        {
            case storage::sqlite::WalCheckpointMode::PASSIVE:
                return "PASSIVE";
            case storage::sqlite::WalCheckpointMode::FULL:
                return "FULL";
            case storage::sqlite::WalCheckpointMode::TRUNCATE:
                return "TRUNCATE";
        }
        return "UNKNOWN";
    }

    int toSqliteCheckpointMode(const storage::sqlite::WalCheckpointMode mode)
    {
        switch(mode)
        {
            case storage::sqlite::WalCheckpointMode::PASSIVE:
                return SQLITE_CHECKPOINT_PASSIVE;
            case storage::sqlite::WalCheckpointMode::FULL:
                return SQLITE_CHECKPOINT_FULL;
            case storage::sqlite::WalCheckpointMode::TRUNCATE:
                return SQLITE_CHECKPOINT_TRUNCATE;
        }
        return SQLITE_CHECKPOINT_PASSIVE;
    }
}