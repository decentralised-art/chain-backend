#include <algorithm>
#include <chrono>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <format>

#include <spdlog/spdlog.h>

#include "utils.hpp"
#include "sqlite/statement.hpp"
#include "sqlite/column.hpp"
#include "sqlite/exec.hpp"

#include "sqlite_hot_store.hpp"

namespace dcn::events
{
    // Bumped whenever a table definition changes; a database of another version is refused.
    constexpr int HOT_SCHEMA_VERSION = 1;

    // Column list shared by readChangesSince / readDeadLetters; must stay in sync
    // with _changeRecordFromRow below.
    constexpr const char * CHANGE_RECORD_COLUMNS =
        "n.source_id, n.chain_namespace, r.address, n.chain_id, n.block_hash, n.log_index, n.change_seq, n.event_type, n.state, "
        "n.name, n.owner, r.data_hex, r.topic0, r.topic1, r.topic2, r.topic3, n.block_number, n.tx_index, "
        "n.tx_hash, n.block_time, n.content_hash, n.metadata_hash, n.runtime_code_hash, n.entity_kind, n.old_owner, n.entity_address ";

    static ChangeRecord _changeRecordFromRow(sqlite3_stmt * stmt)
    {
        ChangeRecord rec{};
        rec.source_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.chain_namespace = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.registry_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.chain_id = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        rec.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        rec.log_index = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 5));
        rec.change_seq = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 6));
        rec.event_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        rec.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        rec.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        rec.owner = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        rec.data_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        rec.topics[0] = storage::sqlite::columnTextOptional(stmt, 12);
        rec.topics[1] = storage::sqlite::columnTextOptional(stmt, 13);
        rec.topics[2] = storage::sqlite::columnTextOptional(stmt, 14);
        rec.topics[3] = storage::sqlite::columnTextOptional(stmt, 15);
        rec.block_number = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 16));
        rec.tx_index = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 17));
        rec.tx_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18));
        rec.block_time = storage::sqlite::columnInt64Optional(stmt, 19);
        rec.content_hash = storage::sqlite::columnTextOptional(stmt, 20);
        rec.metadata_hash = storage::sqlite::columnTextOptional(stmt, 21);
        rec.runtime_code_hash = storage::sqlite::columnTextOptional(stmt, 22);
        if(const auto kind = storage::sqlite::columnTextOptional(stmt, 23))
        {
            rec.entity_kind = pt::parseEntityKind(*kind);
        }
        rec.old_owner = storage::sqlite::columnTextOptional(stmt, 24);
        rec.entity_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 25));
        return rec;
    }

    struct EventKey
    {
        std::string block_hash;
        std::int64_t log_index = 0;

        bool operator==(const EventKey & other) const
        {
            return block_hash == other.block_hash && log_index == other.log_index;
        }
    };

    struct EventKeyHash
    {
        std::size_t operator()(const EventKey & key) const
        {
            const std::size_t h1 = std::hash<std::string>{}(key.block_hash);
            const std::size_t h2 = std::hash<std::int64_t>{}(key.log_index);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };
    SQLiteHotStore::SQLiteHotStore(const std::filesystem::path& hot_db_path,
                                   const std::uint64_t default_chain_id)
        : _hot_db_path(hot_db_path)
        , _default_chain_id(default_chain_id)
    {
        if (!_hot_db_path.parent_path().empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(_hot_db_path.parent_path(), ec);
            if (ec)
            {
                throw std::runtime_error(std::format(
                    "Failed to create events DB directory '{}': {}", _hot_db_path.parent_path().string(), ec.message()));
            }
        }

        if (utils::likelyNetworkPath(_hot_db_path))
        {
            throw std::runtime_error(std::format("Refusing to open events hot DB on likely network filesystem path '{}'",
                                                 _hot_db_path.string()));
        }

        const int write_rc = sqlite3_open_v2(_hot_db_path.string().c_str(),
                                             &_write_db,
                                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                             nullptr);
        if (write_rc != SQLITE_OK)
        {
            const std::string err = (_write_db == nullptr) ? "sqlite open failed" : sqlite3_errmsg(_write_db);
            if (_write_db != nullptr)
            {
                sqlite3_close(_write_db);
                _write_db = nullptr;
            }
            throw std::runtime_error(err);
        }

        const int read_rc = sqlite3_open_v2(_hot_db_path.string().c_str(),
                                            &_read_db,
                                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                            nullptr);
        if (read_rc != SQLITE_OK)
        {
            const std::string err = (_read_db == nullptr) ? "sqlite open failed" : sqlite3_errmsg(_read_db);
            if (_read_db != nullptr)
            {
                sqlite3_close(_read_db);
                _read_db = nullptr;
            }
            sqlite3_close(_write_db);
            _write_db = nullptr;
            throw std::runtime_error(err);
        }

        sqlite3_busy_timeout(_write_db, 10'000);
        sqlite3_busy_timeout(_read_db, 10'000);

        if (!storage::sqlite::exec(_write_db, "PRAGMA journal_mode=WAL;") 
            || !storage::sqlite::exec(_write_db, "PRAGMA synchronous=NORMAL;") 
            || !storage::sqlite::exec(_write_db, "PRAGMA temp_store=MEMORY;") 
            || !storage::sqlite::exec(_write_db, "PRAGMA foreign_keys=OFF;") 
            || !storage::sqlite::exec(_write_db, "PRAGMA wal_autocheckpoint=0;") 
            || !storage::sqlite::exec(_write_db, "PRAGMA busy_timeout=10000;"))
        {
            throw std::runtime_error("Failed to configure events hot write DB pragmas");
        }

        if (!storage::sqlite::exec(_read_db, "PRAGMA journal_mode=WAL;") 
            || !storage::sqlite::exec(_read_db, "PRAGMA synchronous=NORMAL;") 
            || !storage::sqlite::exec(_read_db, "PRAGMA temp_store=MEMORY;") 
            || !storage::sqlite::exec(_read_db, "PRAGMA foreign_keys=OFF;") 
            || !storage::sqlite::exec(_read_db, "PRAGMA busy_timeout=10000;") 
            || !storage::sqlite::exec(_read_db, "PRAGMA query_only=ON;"))
        {
            throw std::runtime_error("Failed to configure events hot read DB pragmas");
        }

        if (!storage::sqlite::ensureSchemaVersion(_write_db, HOT_SCHEMA_VERSION, "events hot")
            || !_initializeHotSchema())
        {
            throw std::runtime_error("Failed to initialize events hot DB schema");
        }
    }

    SQLiteHotStore::~SQLiteHotStore()
    {
        if (_read_db != nullptr)
        {
            sqlite3_close(_read_db);
            _read_db = nullptr;
        }
        if (_write_db != nullptr)
        {
            sqlite3_close(_write_db);
            _write_db = nullptr;
        }
    }

    storage::sqlite::WalCheckpointStats SQLiteHotStore::checkpointWal(storage::sqlite::WalCheckpointMode mode)
    {
        storage::sqlite::WalCheckpointStats stats{};

        const auto mode_str = storage::sqlite::checkpointModeToString(mode);

        try
        {
            storage::sqlite::Statement stmt(_write_db, std::format("PRAGMA wal_checkpoint({});", mode_str).c_str());

            if (stmt.step() == SQLITE_ROW)
            {
                stats.ok = true;
                stats.busy = sqlite3_column_int(stmt.get(), 0);
                stats.log_frames = sqlite3_column_int(stmt.get(), 1);
                stats.checkpointed_frames = sqlite3_column_int(stmt.get(), 2);
            }
        }
        catch (const std::exception& e)
        {
            spdlog::warn("Events WAL checkpoint mode={} failed during prepare/step: {}", mode_str, e.what());
            return stats;
        }

        std::error_code ec;
        const std::filesystem::path wal_path = std::filesystem::path(_hot_db_path.string() + "-wal");
        if (std::filesystem::exists(wal_path, ec) && !ec)
        {
            stats.wal_bytes = std::filesystem::file_size(wal_path, ec);
            if (ec)
            {
                stats.wal_bytes = 0;
            }
        }

        if (stats.ok)
        {
            spdlog::debug(
                "Events WAL checkpoint mode={} busy={} log_frames={} checkpointed_frames={} wal_bytes={}",
                mode_str,
                stats.busy,
                stats.log_frames,
                stats.checkpointed_frames,
                stats.wal_bytes);
        }
        else
        {
            spdlog::warn("Events WAL checkpoint mode={} failed", mode_str);
        }

        return stats;
    }

    std::optional<std::uint64_t> SQLiteHotStore::loadNextCursor(
        const std::string & source_id,
        const CursorKind kind)
    {
        storage::sqlite::Statement stmt(
            _write_db,
            "SELECT next_cursor FROM source_resume WHERE source_id=?1 AND cursor_kind=?2;");
        sqlite3_bind_text(stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
        const std::string_view cursor_kind = toString(kind);
        sqlite3_bind_text(stmt.get(), 2, cursor_kind.data(), static_cast<int>(cursor_kind.size()), SQLITE_TRANSIENT);

        if (stmt.step() != SQLITE_ROW)
        {
            return std::nullopt;
        }

        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
    }

    bool SQLiteHotStore::ingestBatch(
        const std::string & source_id,
        const std::uint64_t chain_id,
        const std::vector<DecodedEvent>& events,
        const std::vector<ChainBlockInfo>& block_infos,
        const std::uint64_t next_cursor,
        const std::int64_t now_ms)
    {
        std::vector<RawChainLog> raw_events;
        raw_events.reserve(events.size());

        for (const DecodedEvent& ev : events)
        {
            raw_events.push_back(ev.raw);
        }

        return ingestBatch(
            source_id,
            CursorKind::NEXT_BLOCK,
            chain_id,
            raw_events,
            events,
            block_infos,
            next_cursor,
            now_ms);
    }

    bool SQLiteHotStore::ingestBatch(
        const std::string & source_id,
        const CursorKind kind,
        const std::uint64_t chain_id,
        const std::vector<RawChainLog>& raw_events,
        const std::vector<DecodedEvent>& decoded_events,
        const std::vector<ChainBlockInfo>& block_infos,
        const std::uint64_t next_cursor,
        const std::int64_t now_ms,
        const std::optional<ScannedBlockRange> & scanned_range)
    {
        if (!storage::sqlite::exec(_write_db, "BEGIN IMMEDIATE TRANSACTION;"))
        {
            return false;
        }

        try
        {
            std::unordered_map<EventKey, const DecodedEvent*, EventKeyHash> decoded_by_key;
            decoded_by_key.reserve(decoded_events.size());
            for (const DecodedEvent& event : decoded_events)
            {
                decoded_by_key.insert_or_assign(EventKey{event.raw.block_hash, event.raw.log_index}, &event);
            }

            storage::sqlite::Statement block_window_stmt(
                _write_db,
                "INSERT INTO reorg_window(chain_id, block_number, block_hash, parent_hash, seen_at_ms, source_id) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
                "ON CONFLICT(source_id, block_number) DO UPDATE SET "
                "block_hash=excluded.block_hash, parent_hash=excluded.parent_hash, seen_at_ms=excluded.seen_at_ms;");

            storage::sqlite::Statement select_reorg_removed_norm_stmt(
                _write_db,
                "SELECT block_hash, log_index "
                "FROM normalized_events_hot "
                "WHERE chain_id=?1 AND block_number=?2 AND block_hash<>?3 AND source_id=?4 AND state!='removed';");

            storage::sqlite::Statement mark_reorg_removed_norm_stmt(
                _write_db,
                "UPDATE normalized_events_hot "
                "SET state='removed', updated_at_ms=?1, change_seq=?2 "
                "WHERE chain_id=?3 AND block_hash=?4 AND log_index=?5 AND source_id=?6;");

            storage::sqlite::Statement mark_reorg_removed_raw_stmt(
                _write_db,
                "UPDATE raw_events_hot "
                "SET removed=1, state='removed', removed_at_ms=COALESCE(removed_at_ms, ?1), updated_at_ms=?2 "
                "WHERE chain_id=?3 AND block_number=?4 AND block_hash<>?5 AND source_id=?6 AND state!='removed';");

            for (const ChainBlockInfo& block_info : block_infos)
            {
                sqlite3_bind_int64(block_window_stmt.get(), 1, static_cast<sqlite3_int64>(block_info.chain_id));
                sqlite3_bind_int64(block_window_stmt.get(), 2, static_cast<sqlite3_int64>(block_info.block_number));
                sqlite3_bind_text(
                    block_window_stmt.get(),
                    3,
                    block_info.block_hash.c_str(),
                    static_cast<int>(block_info.block_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    block_window_stmt.get(),
                    4,
                    block_info.parent_hash.c_str(),
                    static_cast<int>(block_info.parent_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(block_window_stmt.get(), 5, static_cast<sqlite3_int64>(block_info.seen_at_ms));
                sqlite3_bind_text(block_window_stmt.get(), 6, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                if (block_window_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
                sqlite3_reset(block_window_stmt.get());
                sqlite3_clear_bindings(block_window_stmt.get());

                sqlite3_bind_int64(select_reorg_removed_norm_stmt.get(), 1, static_cast<sqlite3_int64>(block_info.chain_id));
                sqlite3_bind_int64(
                    select_reorg_removed_norm_stmt.get(),
                    2,
                    static_cast<sqlite3_int64>(block_info.block_number));
                sqlite3_bind_text(
                    select_reorg_removed_norm_stmt.get(),
                    3,
                    block_info.block_hash.c_str(),
                    static_cast<int>(block_info.block_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(select_reorg_removed_norm_stmt.get(), 4, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                std::vector<std::pair<std::string, std::int64_t>> reorg_removed_keys;
                {
                    int sel_rc = SQLITE_OK;
                    while ((sel_rc = select_reorg_removed_norm_stmt.step()) == SQLITE_ROW)
                    {
                        std::string bh = reinterpret_cast<const char*>(
                            sqlite3_column_text(select_reorg_removed_norm_stmt.get(), 0));
                        std::int64_t li = static_cast<std::int64_t>(
                            sqlite3_column_int64(select_reorg_removed_norm_stmt.get(), 1));
                        reorg_removed_keys.emplace_back(std::move(bh), li);
                    }
                    if (sel_rc != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }
                sqlite3_reset(select_reorg_removed_norm_stmt.get());
                sqlite3_clear_bindings(select_reorg_removed_norm_stmt.get());

                for (const auto& [bh, li] : reorg_removed_keys)
                {
                    sqlite3_bind_int64(mark_reorg_removed_norm_stmt.get(), 1, static_cast<sqlite3_int64>(now_ms));
                    sqlite3_bind_int64(mark_reorg_removed_norm_stmt.get(), 2, static_cast<sqlite3_int64>(_nextChangeSeq()));
                    sqlite3_bind_int64(mark_reorg_removed_norm_stmt.get(), 3, static_cast<sqlite3_int64>(block_info.chain_id));
                    sqlite3_bind_text(
                        mark_reorg_removed_norm_stmt.get(),
                        4,
                        bh.c_str(),
                        static_cast<int>(bh.size()),
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int64(mark_reorg_removed_norm_stmt.get(), 5, static_cast<sqlite3_int64>(li));
                    sqlite3_bind_text(mark_reorg_removed_norm_stmt.get(), 6, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                    if (mark_reorg_removed_norm_stmt.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                    sqlite3_reset(mark_reorg_removed_norm_stmt.get());
                    sqlite3_clear_bindings(mark_reorg_removed_norm_stmt.get());
                }

                sqlite3_bind_int64(mark_reorg_removed_raw_stmt.get(), 1, static_cast<sqlite3_int64>(now_ms));
                sqlite3_bind_int64(mark_reorg_removed_raw_stmt.get(), 2, static_cast<sqlite3_int64>(now_ms));
                sqlite3_bind_int64(mark_reorg_removed_raw_stmt.get(), 3, static_cast<sqlite3_int64>(block_info.chain_id));
                sqlite3_bind_int64(
                    mark_reorg_removed_raw_stmt.get(),
                    4,
                    static_cast<sqlite3_int64>(block_info.block_number));
                sqlite3_bind_text(
                    mark_reorg_removed_raw_stmt.get(),
                    5,
                    block_info.block_hash.c_str(),
                    static_cast<int>(block_info.block_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(mark_reorg_removed_raw_stmt.get(), 6, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                if (mark_reorg_removed_raw_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
                sqlite3_reset(mark_reorg_removed_raw_stmt.get());
                sqlite3_clear_bindings(mark_reorg_removed_raw_stmt.get());
            }

            storage::sqlite::Statement raw_stmt(
                _write_db,
                "INSERT INTO raw_events_hot("
                "chain_id, block_hash, log_index, tx_hash, block_number, tx_index, block_time, "
                "address, topic0, topic1, topic2, topic3, data_hex, removed, state, seen_at_ms, "
                "updated_at_ms, removed_at_ms, source_id, chain_namespace"
                ") VALUES("
                "?1, ?2, ?3, ?4, ?5, ?6, ?7, "
                "?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20"
                ") "
                "ON CONFLICT(source_id, block_hash, log_index) DO UPDATE SET "
                "tx_hash=excluded.tx_hash, block_number=excluded.block_number, tx_index=excluded.tx_index, "
                "block_time=excluded.block_time, "
                "address=excluded.address, topic0=excluded.topic0, topic1=excluded.topic1, "
                "topic2=excluded.topic2, topic3=excluded.topic3, "
                "data_hex=excluded.data_hex, removed=excluded.removed, "
                "state=CASE "
                "WHEN excluded.state='removed' THEN 'removed' "
                "WHEN raw_events_hot.state='finalized' THEN 'finalized' "
                "WHEN raw_events_hot.state='safe' AND excluded.state='observed' THEN 'safe' "
                "ELSE excluded.state END, "
                "seen_at_ms=MIN(raw_events_hot.seen_at_ms, excluded.seen_at_ms), "
                "updated_at_ms=excluded.updated_at_ms, "
                "removed_at_ms=CASE "
                "WHEN excluded.state='removed' OR excluded.removed=1 THEN "
                "COALESCE(raw_events_hot.removed_at_ms, excluded.removed_at_ms) "
                "ELSE NULL END "
                "WHERE raw_events_hot.tx_hash<>excluded.tx_hash "
                "OR raw_events_hot.block_number<>excluded.block_number "
                "OR raw_events_hot.tx_index<>excluded.tx_index "
                "OR raw_events_hot.block_time IS NOT excluded.block_time "
                "OR raw_events_hot.address<>excluded.address "
                "OR raw_events_hot.topic0<>excluded.topic0 "
                "OR raw_events_hot.topic1 IS NOT excluded.topic1 "
                "OR raw_events_hot.topic2 IS NOT excluded.topic2 "
                "OR raw_events_hot.topic3 IS NOT excluded.topic3 "
                "OR raw_events_hot.data_hex<>excluded.data_hex "
                "OR raw_events_hot.removed<>excluded.removed "
                "OR raw_events_hot.state<>CASE "
                "WHEN excluded.state='removed' THEN 'removed' "
                "WHEN raw_events_hot.state='finalized' THEN 'finalized' "
                "WHEN raw_events_hot.state='safe' AND excluded.state='observed' THEN 'safe' "
                "ELSE excluded.state END "
                "OR (excluded.state='removed' AND raw_events_hot.removed_at_ms IS NULL);");

            storage::sqlite::Statement normalized_stmt(
                _write_db,
                "INSERT INTO normalized_events_hot("
                "chain_id, block_hash, log_index, tx_hash, block_number, tx_index, block_time, "
                "event_type, name, caller, owner, entity_address, args_count, format_hash, state, "
                "seen_at_ms, updated_at_ms, change_seq, content_hash, metadata_hash, runtime_code_hash, "
                "entity_kind, old_owner, source_id, chain_namespace"
                ") VALUES("
                "?1, ?2, ?3, ?4, ?5, ?6, ?7, "
                "?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, "
                "?19, ?20, ?21, ?22, ?23, ?24, ?25"
                ") "
                "ON CONFLICT(source_id, block_hash, log_index) DO UPDATE SET "
                "tx_hash=excluded.tx_hash, block_number=excluded.block_number, tx_index=excluded.tx_index, "
                "block_time=excluded.block_time, "
                "event_type=excluded.event_type, name=excluded.name, caller=excluded.caller, owner=excluded.owner, "
                "entity_address=excluded.entity_address, "
                "args_count=excluded.args_count, format_hash=excluded.format_hash, "
                "content_hash=excluded.content_hash, metadata_hash=excluded.metadata_hash, "
                "runtime_code_hash=excluded.runtime_code_hash, entity_kind=excluded.entity_kind, old_owner=excluded.old_owner, "
                "state=CASE "
                "WHEN excluded.state='removed' THEN 'removed' "
                "WHEN normalized_events_hot.state='finalized' THEN 'finalized' "
                "WHEN normalized_events_hot.state='safe' AND excluded.state='observed' THEN 'safe' "
                "ELSE excluded.state END, "
                "seen_at_ms=MIN(normalized_events_hot.seen_at_ms, excluded.seen_at_ms), "
                "updated_at_ms=excluded.updated_at_ms, "
                "change_seq=excluded.change_seq "
                "WHERE normalized_events_hot.tx_hash<>excluded.tx_hash "
                "OR normalized_events_hot.block_number<>excluded.block_number "
                "OR normalized_events_hot.tx_index<>excluded.tx_index "
                "OR normalized_events_hot.block_time IS NOT excluded.block_time "
                "OR normalized_events_hot.event_type<>excluded.event_type "
                "OR normalized_events_hot.name<>excluded.name "
                "OR normalized_events_hot.caller<>excluded.caller "
                "OR normalized_events_hot.owner<>excluded.owner "
                "OR normalized_events_hot.entity_address<>excluded.entity_address "
                "OR normalized_events_hot.args_count IS NOT excluded.args_count "
                "OR normalized_events_hot.format_hash IS NOT excluded.format_hash "
                "OR normalized_events_hot.content_hash IS NOT excluded.content_hash "
                "OR normalized_events_hot.metadata_hash IS NOT excluded.metadata_hash "
                "OR normalized_events_hot.runtime_code_hash IS NOT excluded.runtime_code_hash "
                "OR normalized_events_hot.entity_kind IS NOT excluded.entity_kind "
                "OR normalized_events_hot.old_owner IS NOT excluded.old_owner "
                "OR normalized_events_hot.state<>CASE "
                "WHEN excluded.state='removed' THEN 'removed' "
                "WHEN normalized_events_hot.state='finalized' THEN 'finalized' "
                "WHEN normalized_events_hot.state='safe' AND excluded.state='observed' THEN 'safe' "
                "ELSE excluded.state END;");

            storage::sqlite::Statement decode_failure_stmt(
                _write_db,
                "INSERT INTO decode_failures_hot("
                "chain_id, block_hash, log_index, last_error, attempts, first_seen_at_ms, last_seen_at_ms, retryable, dead_letter, source_id"
                ") VALUES("
                "?1, ?2, ?3, ?4, 1, ?5, ?6, 1, 0, ?7"
                ") ON CONFLICT(source_id, block_hash, log_index) DO UPDATE SET "
                "last_error=excluded.last_error, "
                "attempts=decode_failures_hot.attempts + 1, "
                "last_seen_at_ms=excluded.last_seen_at_ms, "
                "retryable=CASE WHEN decode_failures_hot.attempts + 1 >= 16 THEN 0 ELSE 1 END, "
                "dead_letter=CASE WHEN decode_failures_hot.attempts + 1 >= 16 THEN 1 ELSE 0 END;");

            storage::sqlite::Statement clear_decode_failure_stmt(
                _write_db,
                "DELETE FROM decode_failures_hot WHERE chain_id=?1 AND block_hash=?2 AND log_index=?3 AND source_id=?4;");

            storage::sqlite::Statement force_removed_norm_stmt(
                _write_db,
                "UPDATE normalized_events_hot "
                "SET state='removed', updated_at_ms=?1, change_seq=?5 "
                "WHERE chain_id=?2 AND block_hash=?3 AND log_index=?4 AND source_id=?6 AND state!='removed';");

            for (const RawChainLog& raw_event : raw_events)
            {
                const auto decoded_it = decoded_by_key.find(EventKey{raw_event.block_hash, raw_event.log_index});
                const DecodedEvent* decoded = (decoded_it == decoded_by_key.end()) ? nullptr : decoded_it->second;

                const std::string decoded_state =
                    (decoded != nullptr)
                        ? toString(decoded->state)
                        : std::string(OBSERVED_STATE);

                const bool incoming_removed = raw_event.removed || decoded_state == REMOVED_STATE;
                const std::string effective_state =
                    incoming_removed ? std::string(REMOVED_STATE) : decoded_state;
                const std::optional<std::int64_t> removed_at =
                    incoming_removed ? std::optional<std::int64_t>(now_ms) : std::nullopt;

                // topic0 is persisted in a NOT NULL column; missing topic0 is normalized to empty string.
                const std::string topic0 = raw_event.topics[0].value_or(std::string());
                const std::optional<std::string> topic1 = raw_event.topics[1];
                const std::optional<std::string> topic2 = raw_event.topics[2];
                const std::optional<std::string> topic3 = raw_event.topics[3];

                sqlite3_bind_int64(raw_stmt.get(), 1, static_cast<sqlite3_int64>(chain_id));
                sqlite3_bind_text(
                    raw_stmt.get(),
                    2,
                    raw_event.block_hash.c_str(),
                    static_cast<int>(raw_event.block_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(raw_stmt.get(), 3, static_cast<sqlite3_int64>(raw_event.log_index));
                sqlite3_bind_text(
                    raw_stmt.get(),
                    4,
                    raw_event.tx_hash.c_str(),
                    static_cast<int>(raw_event.tx_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(raw_stmt.get(), 5, static_cast<sqlite3_int64>(raw_event.block_number));
                sqlite3_bind_int64(raw_stmt.get(), 6, static_cast<sqlite3_int64>(raw_event.tx_index));
                storage::sqlite::bindOptionalInt64(raw_stmt.get(), 7, raw_event.block_time);
                sqlite3_bind_text(
                    raw_stmt.get(),
                    8,
                    raw_event.address.c_str(),
                    static_cast<int>(raw_event.address.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    raw_stmt.get(),
                    9,
                    topic0.c_str(),
                    static_cast<int>(topic0.size()),
                    SQLITE_TRANSIENT);
                storage::sqlite::bindOptionalText(raw_stmt.get(), 10, topic1);
                storage::sqlite::bindOptionalText(raw_stmt.get(), 11, topic2);
                storage::sqlite::bindOptionalText(raw_stmt.get(), 12, topic3);
                sqlite3_bind_text(
                    raw_stmt.get(),
                    13,
                    raw_event.data_hex.c_str(),
                    static_cast<int>(raw_event.data_hex.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int(raw_stmt.get(), 14, incoming_removed ? 1 : 0);
                sqlite3_bind_text(
                    raw_stmt.get(),
                    15,
                    effective_state.c_str(),
                    static_cast<int>(effective_state.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(raw_stmt.get(), 16, static_cast<sqlite3_int64>(raw_event.seen_at_ms));
                sqlite3_bind_int64(raw_stmt.get(), 17, static_cast<sqlite3_int64>(now_ms));
                storage::sqlite::bindOptionalInt64(raw_stmt.get(), 18, removed_at);
                sqlite3_bind_text(raw_stmt.get(), 19, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(raw_stmt.get(), 20, raw_event.chain_namespace.c_str(), static_cast<int>(raw_event.chain_namespace.size()), SQLITE_TRANSIENT);

                if (raw_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }

                sqlite3_reset(raw_stmt.get());
                sqlite3_clear_bindings(raw_stmt.get());

                if (decoded == nullptr)
                {
                    if (incoming_removed)
                    {
                        sqlite3_bind_int64(force_removed_norm_stmt.get(), 1, static_cast<sqlite3_int64>(now_ms));
                        sqlite3_bind_int64(force_removed_norm_stmt.get(), 2, static_cast<sqlite3_int64>(chain_id));
                        sqlite3_bind_text(
                            force_removed_norm_stmt.get(),
                            3,
                            raw_event.block_hash.c_str(),
                            static_cast<int>(raw_event.block_hash.size()),
                            SQLITE_TRANSIENT);
                        sqlite3_bind_int64(
                            force_removed_norm_stmt.get(),
                            4,
                            static_cast<sqlite3_int64>(raw_event.log_index));
                        sqlite3_bind_int64(
                            force_removed_norm_stmt.get(),
                            5,
                            static_cast<sqlite3_int64>(_nextChangeSeq()));
                        sqlite3_bind_text(force_removed_norm_stmt.get(), 6, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                        if (force_removed_norm_stmt.step() != SQLITE_DONE)
                        {
                            throw std::runtime_error(sqlite3_errmsg(_write_db));
                        }
                        sqlite3_reset(force_removed_norm_stmt.get());
                        sqlite3_clear_bindings(force_removed_norm_stmt.get());

                        sqlite3_bind_int64(clear_decode_failure_stmt.get(), 1, static_cast<sqlite3_int64>(chain_id));
                        sqlite3_bind_text(
                            clear_decode_failure_stmt.get(),
                            2,
                            raw_event.block_hash.c_str(),
                            static_cast<int>(raw_event.block_hash.size()),
                            SQLITE_TRANSIENT);
                        sqlite3_bind_int64(
                            clear_decode_failure_stmt.get(),
                            3,
                            static_cast<sqlite3_int64>(raw_event.log_index));
                        sqlite3_bind_text(clear_decode_failure_stmt.get(), 4, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                        if (clear_decode_failure_stmt.step() != SQLITE_DONE)
                        {
                            throw std::runtime_error(sqlite3_errmsg(_write_db));
                        }
                        sqlite3_reset(clear_decode_failure_stmt.get());
                        sqlite3_clear_bindings(clear_decode_failure_stmt.get());
                        continue;
                    }

                    sqlite3_bind_int64(decode_failure_stmt.get(), 1, static_cast<sqlite3_int64>(chain_id));
                    sqlite3_bind_text(
                        decode_failure_stmt.get(),
                        2,
                        raw_event.block_hash.c_str(),
                        static_cast<int>(raw_event.block_hash.size()),
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int64(
                        decode_failure_stmt.get(),
                        3,
                        static_cast<sqlite3_int64>(raw_event.log_index));
                    static constexpr std::string_view DECODE_ERROR = "decode_failed";
                    sqlite3_bind_text(
                        decode_failure_stmt.get(),
                        4,
                        DECODE_ERROR.data(),
                        static_cast<int>(DECODE_ERROR.size()),
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int64(
                        decode_failure_stmt.get(),
                        5,
                        static_cast<sqlite3_int64>(raw_event.seen_at_ms));
                    sqlite3_bind_int64(
                        decode_failure_stmt.get(),
                        6,
                        static_cast<sqlite3_int64>(now_ms));
                    sqlite3_bind_text(decode_failure_stmt.get(), 7, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                    if (decode_failure_stmt.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                    sqlite3_reset(decode_failure_stmt.get());
                    sqlite3_clear_bindings(decode_failure_stmt.get());
                    continue;
                }

                sqlite3_bind_int64(clear_decode_failure_stmt.get(), 1, static_cast<sqlite3_int64>(chain_id));
                sqlite3_bind_text(
                    clear_decode_failure_stmt.get(),
                    2,
                    raw_event.block_hash.c_str(),
                    static_cast<int>(raw_event.block_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(
                    clear_decode_failure_stmt.get(),
                    3,
                    static_cast<sqlite3_int64>(raw_event.log_index));
                sqlite3_bind_text(clear_decode_failure_stmt.get(), 4, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);

                if (clear_decode_failure_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
                sqlite3_reset(clear_decode_failure_stmt.get());
                sqlite3_clear_bindings(clear_decode_failure_stmt.get());

                const std::string event_type_str = toString(decoded->event_type);
                const std::string state_str = effective_state;

                sqlite3_bind_int64(normalized_stmt.get(), 1, static_cast<sqlite3_int64>(chain_id));
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    2,
                    decoded->raw.block_hash.c_str(),
                    static_cast<int>(decoded->raw.block_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(normalized_stmt.get(), 3, static_cast<sqlite3_int64>(decoded->raw.log_index));
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    4,
                    decoded->raw.tx_hash.c_str(),
                    static_cast<int>(decoded->raw.tx_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(normalized_stmt.get(), 5, static_cast<sqlite3_int64>(decoded->raw.block_number));
                sqlite3_bind_int64(normalized_stmt.get(), 6, static_cast<sqlite3_int64>(decoded->raw.tx_index));
                storage::sqlite::bindOptionalInt64(normalized_stmt.get(), 7, decoded->raw.block_time);
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    8,
                    event_type_str.c_str(),
                    static_cast<int>(event_type_str.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    9,
                    decoded->name.c_str(),
                    static_cast<int>(decoded->name.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    10,
                    decoded->caller.c_str(),
                    static_cast<int>(decoded->caller.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    11,
                    decoded->owner.c_str(),
                    static_cast<int>(decoded->owner.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    12,
                    decoded->entity_address.c_str(),
                    static_cast<int>(decoded->entity_address.size()),
                    SQLITE_TRANSIENT);

                if (decoded->args_count.has_value())
                {
                    sqlite3_bind_int64(
                        normalized_stmt.get(),
                        13,
                        static_cast<sqlite3_int64>(*decoded->args_count));
                }
                else
                {
                    sqlite3_bind_null(normalized_stmt.get(), 13);
                }

                storage::sqlite::bindOptionalText(normalized_stmt.get(), 14, decoded->format_hash);
                sqlite3_bind_text(
                    normalized_stmt.get(),
                    15,
                    state_str.c_str(),
                    static_cast<int>(state_str.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(
                    normalized_stmt.get(),
                    16,
                    static_cast<sqlite3_int64>(decoded->raw.seen_at_ms));
                sqlite3_bind_int64(normalized_stmt.get(), 17, static_cast<sqlite3_int64>(now_ms));
                sqlite3_bind_int64(normalized_stmt.get(), 18, static_cast<sqlite3_int64>(_nextChangeSeq()));
                storage::sqlite::bindOptionalText(normalized_stmt.get(), 19, decoded->content_hash);
                storage::sqlite::bindOptionalText(normalized_stmt.get(), 20, decoded->metadata_hash);
                storage::sqlite::bindOptionalText(normalized_stmt.get(), 21, decoded->runtime_code_hash);
                storage::sqlite::bindOptionalText(
                    normalized_stmt.get(),
                    22,
                    decoded->entity_kind
                        ? std::optional<std::string>(pt::entityKindName(*decoded->entity_kind))
                        : std::nullopt);
                storage::sqlite::bindOptionalText(normalized_stmt.get(), 23, decoded->old_owner);
                sqlite3_bind_text(normalized_stmt.get(), 24, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(normalized_stmt.get(), 25, decoded->raw.chain_namespace.c_str(), static_cast<int>(decoded->raw.chain_namespace.size()), SQLITE_TRANSIENT);

                if (normalized_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }

                sqlite3_reset(normalized_stmt.get());
                sqlite3_clear_bindings(normalized_stmt.get());

                const int normalized_changed = sqlite3_changes(_write_db);
                if (normalized_changed == 0)
                {
                    continue;
                }
            }

            if(scanned_range.has_value() && scanned_range->to_block >= scanned_range->from_block)
            {
                _reconcileScannedRange(source_id, *scanned_range, raw_events, now_ms);
            }

            storage::sqlite::Statement resume_stmt(
                _write_db,
                "INSERT INTO source_resume(source_id, cursor_kind, next_cursor, updated_at_ms) "
                "VALUES(?1, ?2, ?3, ?4) "
                "ON CONFLICT(source_id) DO UPDATE SET "
                "cursor_kind=excluded.cursor_kind, next_cursor=excluded.next_cursor, updated_at_ms=excluded.updated_at_ms;");

            sqlite3_bind_text(resume_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            const std::string_view cursor_kind = toString(kind);
            sqlite3_bind_text(resume_stmt.get(), 2, cursor_kind.data(), static_cast<int>(cursor_kind.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(resume_stmt.get(), 3, static_cast<sqlite3_int64>(next_cursor));
            sqlite3_bind_int64(resume_stmt.get(), 4, static_cast<sqlite3_int64>(now_ms));

            if (resume_stmt.step() != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }

            if (!storage::sqlite::exec(_write_db, "COMMIT;"))
            {
                throw std::runtime_error("commit failed");
            }

            return true;
        }
        catch (const std::exception& e)
        {
            spdlog::error("Events ingestBatch failed: {}", e.what());
            (void)storage::sqlite::exec(_write_db, "ROLLBACK;");
            return false;
        }
    }

    void SQLiteHotStore::_reconcileScannedRange(
        const std::string & source_id,
        const ScannedBlockRange & range,
        const std::vector<RawChainLog> & returned_logs,
        const std::int64_t now_ms)
    {
        // The caller scanned this block span completely, so anything still stored as
        // observed or safe inside it that the scan did not return no longer exists on
        // the canonical chain. This is the only check that catches a replacement block
        // holding no PT event at all: comparing block hashes only covers blocks the
        // provider actually returned. Finalized rows are never touched here.
        if(!storage::sqlite::exec(_write_db, "DELETE FROM scan_generation;"))
        {
            throw std::runtime_error("failed to reset scan generation table");
        }

        {
            storage::sqlite::Statement insert_key_stmt(
                _write_db,
                "INSERT OR IGNORE INTO scan_generation(block_hash, log_index) VALUES(?1, ?2);");

            for(const RawChainLog & log : returned_logs)
            {
                sqlite3_bind_text(
                    insert_key_stmt.get(), 1, log.block_hash.c_str(), static_cast<int>(log.block_hash.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(insert_key_stmt.get(), 2, static_cast<sqlite3_int64>(log.log_index));

                if(insert_key_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
                sqlite3_reset(insert_key_stmt.get());
                sqlite3_clear_bindings(insert_key_stmt.get());
            }
        }

        std::vector<std::pair<std::string, std::int64_t>> disappeared;
        {
            storage::sqlite::Statement select_stmt(
                _write_db,
                "SELECT block_hash, log_index FROM normalized_events_hot "
                "WHERE source_id=?1 AND block_number BETWEEN ?2 AND ?3 "
                "AND state IN ('observed','safe') "
                "AND NOT EXISTS (SELECT 1 FROM scan_generation g "
                "WHERE g.block_hash=normalized_events_hot.block_hash "
                "AND g.log_index=normalized_events_hot.log_index);");

            sqlite3_bind_text(select_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(select_stmt.get(), 2, static_cast<sqlite3_int64>(range.from_block));
            sqlite3_bind_int64(select_stmt.get(), 3, static_cast<sqlite3_int64>(range.to_block));

            int rc = SQLITE_OK;
            while((rc = select_stmt.step()) == SQLITE_ROW)
            {
                disappeared.emplace_back(
                    reinterpret_cast<const char *>(sqlite3_column_text(select_stmt.get(), 0)),
                    static_cast<std::int64_t>(sqlite3_column_int64(select_stmt.get(), 1)));
            }
            if(rc != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }
        }

        // Every removal gets its own change_seq so projectors see the reversal as an
        // ordered delta rather than having to diff state.
        storage::sqlite::Statement remove_norm_stmt(
            _write_db,
            "UPDATE normalized_events_hot SET state='removed', updated_at_ms=?1, change_seq=?2 "
            "WHERE source_id=?3 AND block_hash=?4 AND log_index=?5;");

        for(const auto & [block_hash, log_index] : disappeared)
        {
            sqlite3_bind_int64(remove_norm_stmt.get(), 1, static_cast<sqlite3_int64>(now_ms));
            sqlite3_bind_int64(remove_norm_stmt.get(), 2, static_cast<sqlite3_int64>(_nextChangeSeq()));
            sqlite3_bind_text(remove_norm_stmt.get(), 3, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(remove_norm_stmt.get(), 4, block_hash.c_str(), static_cast<int>(block_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(remove_norm_stmt.get(), 5, static_cast<sqlite3_int64>(log_index));

            if(remove_norm_stmt.step() != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }
            sqlite3_reset(remove_norm_stmt.get());
            sqlite3_clear_bindings(remove_norm_stmt.get());
        }

        storage::sqlite::Statement remove_raw_stmt(
            _write_db,
            "UPDATE raw_events_hot SET removed=1, state='removed', "
            "removed_at_ms=COALESCE(removed_at_ms, ?1), updated_at_ms=?1 "
            "WHERE source_id=?2 AND block_number BETWEEN ?3 AND ?4 "
            "AND state IN ('observed','safe') "
            "AND NOT EXISTS (SELECT 1 FROM scan_generation g "
            "WHERE g.block_hash=raw_events_hot.block_hash AND g.log_index=raw_events_hot.log_index);");

        sqlite3_bind_int64(remove_raw_stmt.get(), 1, static_cast<sqlite3_int64>(now_ms));
        sqlite3_bind_text(remove_raw_stmt.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(remove_raw_stmt.get(), 3, static_cast<sqlite3_int64>(range.from_block));
        sqlite3_bind_int64(remove_raw_stmt.get(), 4, static_cast<sqlite3_int64>(range.to_block));

        if(remove_raw_stmt.step() != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_write_db));
        }

        // Blocks in the scanned span that no longer hold any indexed log must not keep
        // a stale header in the reorg window.
        storage::sqlite::Statement prune_window_stmt(
            _write_db,
            "DELETE FROM reorg_window WHERE source_id=?1 AND block_number BETWEEN ?2 AND ?3 "
            "AND NOT EXISTS (SELECT 1 FROM raw_events_hot r "
            "WHERE r.source_id=reorg_window.source_id AND r.block_hash=reorg_window.block_hash "
            "AND r.state!='removed');");

        sqlite3_bind_text(prune_window_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(prune_window_stmt.get(), 2, static_cast<sqlite3_int64>(range.from_block));
        sqlite3_bind_int64(prune_window_stmt.get(), 3, static_cast<sqlite3_int64>(range.to_block));

        if(prune_window_stmt.step() != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_write_db));
        }

        if(!disappeared.empty())
        {
            spdlog::info(
                "Events reorg reconciliation removed {} logs for source={} in blocks [{}, {}]",
                disappeared.size(),
                source_id,
                range.from_block,
                range.to_block);
        }
    }

    bool SQLiteHotStore::applyFinality(
        const std::string & source_id,
        const std::uint64_t chain_id,
        const FinalityHeights& heights,
        const std::int64_t now_ms,
        const std::size_t reorg_window_blocks)
    {
        // -1 means "no block yet": nothing at or below it is promoted.
        const std::int64_t clamped_head = std::max<std::int64_t>(-1, heights.head);
        const std::int64_t clamped_safe = std::clamp<std::int64_t>(heights.safe, -1, clamped_head);
        const std::int64_t clamped_finalized = std::clamp<std::int64_t>(heights.finalized, -1, clamped_safe);

        if (!storage::sqlite::exec(_write_db, "BEGIN IMMEDIATE TRANSACTION;"))
        {
            return false;
        }

        try
        {
            std::int64_t effective_head = clamped_head;
            std::int64_t effective_safe = clamped_safe;
            std::int64_t effective_finalized = clamped_finalized;
            std::string effective_finalized_hash = heights.finalized_hash;

            storage::sqlite::Statement existing_finality_stmt(
                _write_db,
                "SELECT head_block, safe_block, finalized_block, finalized_hash "
                "FROM finality_state WHERE source_id=?1;");

            sqlite3_bind_text(existing_finality_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            if(existing_finality_stmt.step() == SQLITE_ROW)
            {
                const std::int64_t previous_head =
                    static_cast<std::int64_t>(sqlite3_column_int64(existing_finality_stmt.get(), 0));
                const std::int64_t previous_safe =
                    static_cast<std::int64_t>(sqlite3_column_int64(existing_finality_stmt.get(), 1));
                const std::int64_t previous_finalized =
                    static_cast<std::int64_t>(sqlite3_column_int64(existing_finality_stmt.get(), 2));

                effective_head = std::max(previous_head, clamped_head);
                effective_safe = std::max(previous_safe, clamped_safe);
                effective_safe = std::clamp<std::int64_t>(effective_safe, -1, effective_head);
                effective_finalized = std::max(previous_finalized, clamped_finalized);
                effective_finalized = std::clamp<std::int64_t>(effective_finalized, -1, effective_safe);

                // The anchor hash belongs to the finalized height it was reported with.
                if(effective_finalized != clamped_finalized)
                {
                    effective_finalized_hash = storage::sqlite::columnTextOptional(existing_finality_stmt.get(), 3).value_or("");
                }
            }

            storage::sqlite::Statement finality_stmt(
                _write_db,
                "INSERT INTO finality_state(source_id, chain_id, head_block, safe_block, finalized_block, finalized_hash, updated_at_ms) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) "
                "ON CONFLICT(source_id) DO UPDATE SET "
                "head_block=excluded.head_block, safe_block=excluded.safe_block, finalized_block=excluded.finalized_block, "
                "finalized_hash=excluded.finalized_hash, updated_at_ms=excluded.updated_at_ms;");

            sqlite3_bind_text(finality_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(finality_stmt.get(), 2, static_cast<sqlite3_int64>(chain_id));
            sqlite3_bind_int64(finality_stmt.get(), 3, static_cast<sqlite3_int64>(effective_head));
            sqlite3_bind_int64(finality_stmt.get(), 4, static_cast<sqlite3_int64>(effective_safe));
            sqlite3_bind_int64(finality_stmt.get(), 5, static_cast<sqlite3_int64>(effective_finalized));
            sqlite3_bind_text(
                finality_stmt.get(), 6, effective_finalized_hash.c_str(),
                static_cast<int>(effective_finalized_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(finality_stmt.get(), 7, static_cast<sqlite3_int64>(now_ms));

            if (finality_stmt.step() != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }

            {
                storage::sqlite::Statement select_observed_to_safe(
                    _write_db,
                    "SELECT block_hash, log_index "
                    "FROM normalized_events_hot "
                    "WHERE source_id=?1 AND state='observed' AND block_number <= ?2;");

                sqlite3_bind_text(select_observed_to_safe.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(select_observed_to_safe.get(), 2, static_cast<sqlite3_int64>(effective_safe));

                std::vector<std::pair<std::string, std::int64_t>> obs_safe_keys;
                {
                    int sel_rc = SQLITE_OK;
                    while ((sel_rc = select_observed_to_safe.step()) == SQLITE_ROW)
                    {
                        std::string bh = reinterpret_cast<const char*>(
                            sqlite3_column_text(select_observed_to_safe.get(), 0));
                        std::int64_t li = static_cast<std::int64_t>(
                            sqlite3_column_int64(select_observed_to_safe.get(), 1));
                        obs_safe_keys.emplace_back(std::move(bh), li);
                    }
                    if (sel_rc != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }

                storage::sqlite::Statement update_observed_to_safe_norm(
                    _write_db,
                    "UPDATE normalized_events_hot "
                    "SET state='safe', updated_at_ms=?1, change_seq=?2 "
                    "WHERE source_id=?3 AND block_hash=?4 AND log_index=?5;");

                for (const auto& [bh, li] : obs_safe_keys)
                {
                    sqlite3_bind_int64(update_observed_to_safe_norm.get(), 1, static_cast<sqlite3_int64>(now_ms));
                    sqlite3_bind_int64(update_observed_to_safe_norm.get(), 2, static_cast<sqlite3_int64>(_nextChangeSeq()));
                    sqlite3_bind_text(update_observed_to_safe_norm.get(), 3, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        update_observed_to_safe_norm.get(),
                        4,
                        bh.c_str(),
                        static_cast<int>(bh.size()),
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int64(update_observed_to_safe_norm.get(), 5, static_cast<sqlite3_int64>(li));

                    if (update_observed_to_safe_norm.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                    sqlite3_reset(update_observed_to_safe_norm.get());
                    sqlite3_clear_bindings(update_observed_to_safe_norm.get());
                }
            }

            storage::sqlite::Statement update_observed_to_safe_raw(
                _write_db,
                "UPDATE raw_events_hot SET state='safe', updated_at_ms=?1 "
                "WHERE source_id=?2 AND state='observed' AND block_number <= ?3;");

            sqlite3_bind_int64(update_observed_to_safe_raw.get(), 1, static_cast<sqlite3_int64>(now_ms));
            sqlite3_bind_text(update_observed_to_safe_raw.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(update_observed_to_safe_raw.get(), 3, static_cast<sqlite3_int64>(effective_safe));

            if (update_observed_to_safe_raw.step() != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }

            {
                storage::sqlite::Statement select_safe_to_finalized(
                    _write_db,
                    "SELECT block_hash, log_index "
                    "FROM normalized_events_hot "
                    "WHERE source_id=?1 AND state='safe' AND block_number <= ?2;");

                sqlite3_bind_text(select_safe_to_finalized.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(select_safe_to_finalized.get(), 2, static_cast<sqlite3_int64>(effective_finalized));

                std::vector<std::pair<std::string, std::int64_t>> safe_final_keys;
                {
                    int sel_rc = SQLITE_OK;
                    while ((sel_rc = select_safe_to_finalized.step()) == SQLITE_ROW)
                    {
                        std::string bh = reinterpret_cast<const char*>(
                            sqlite3_column_text(select_safe_to_finalized.get(), 0));
                        std::int64_t li = static_cast<std::int64_t>(
                            sqlite3_column_int64(select_safe_to_finalized.get(), 1));
                        safe_final_keys.emplace_back(std::move(bh), li);
                    }
                    if (sel_rc != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }

                storage::sqlite::Statement update_safe_to_finalized_norm(
                    _write_db,
                    "UPDATE normalized_events_hot "
                    "SET state='finalized', updated_at_ms=?1, change_seq=?2 "
                    "WHERE source_id=?3 AND block_hash=?4 AND log_index=?5;");

                for (const auto& [bh, li] : safe_final_keys)
                {
                    sqlite3_bind_int64(update_safe_to_finalized_norm.get(), 1, static_cast<sqlite3_int64>(now_ms));
                    sqlite3_bind_int64(update_safe_to_finalized_norm.get(), 2, static_cast<sqlite3_int64>(_nextChangeSeq()));
                    sqlite3_bind_text(update_safe_to_finalized_norm.get(), 3, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        update_safe_to_finalized_norm.get(),
                        4,
                        bh.c_str(),
                        static_cast<int>(bh.size()),
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int64(update_safe_to_finalized_norm.get(), 5, static_cast<sqlite3_int64>(li));

                    if (update_safe_to_finalized_norm.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                    sqlite3_reset(update_safe_to_finalized_norm.get());
                    sqlite3_clear_bindings(update_safe_to_finalized_norm.get());
                }
            }

            storage::sqlite::Statement update_safe_to_finalized_raw(
                _write_db,
                "UPDATE raw_events_hot SET state='finalized', updated_at_ms=?1 "
                "WHERE source_id=?2 AND state='safe' AND block_number <= ?3;");

            sqlite3_bind_int64(update_safe_to_finalized_raw.get(), 1, static_cast<sqlite3_int64>(now_ms));
            sqlite3_bind_text(update_safe_to_finalized_raw.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(update_safe_to_finalized_raw.get(), 3, static_cast<sqlite3_int64>(effective_finalized));

            if (update_safe_to_finalized_raw.step() != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }

            const std::int64_t floor =
                (effective_head > static_cast<std::int64_t>(reorg_window_blocks))
                    ? (effective_head - static_cast<std::int64_t>(reorg_window_blocks))
                    : 0;

            storage::sqlite::Statement prune_reorg_stmt(
                _write_db,
                "DELETE FROM reorg_window WHERE source_id=?1 AND block_number < ?2;");

            sqlite3_bind_text(prune_reorg_stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(prune_reorg_stmt.get(), 2, static_cast<sqlite3_int64>(floor));

            if (prune_reorg_stmt.step() != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }

            if (!storage::sqlite::exec(_write_db, "COMMIT;"))
            {
                throw std::runtime_error("commit failed");
            }

            return true;
        }
        catch (const std::exception& e)
        {
            spdlog::error("Events applyFinality failed: {}", e.what());
            (void)storage::sqlite::exec(_write_db, "ROLLBACK;");
            return false;
        }
    }

    std::size_t SQLiteHotStore::pruneConsumedRaw(
        const std::string & source_id,
        const std::int64_t watermark,
        const std::int64_t finalized_floor_block,
        const std::size_t batch_limit)
    {
        std::size_t total_pruned = 0;

        try
        {
            storage::sqlite::Statement select_keys(
                _write_db,
                "SELECT block_hash, log_index "
                "FROM normalized_events_hot "
                "WHERE source_id=?1 AND change_seq <= ?2 AND state IN ('finalized','removed') AND block_number <= ?3 "
                "AND dead_letter=0 "
                "LIMIT ?4;");

            while (true)
            {
                sqlite3_bind_text(select_keys.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(select_keys.get(), 2, static_cast<sqlite3_int64>(watermark));
                sqlite3_bind_int64(select_keys.get(), 3, static_cast<sqlite3_int64>(finalized_floor_block));
                sqlite3_bind_int64(select_keys.get(), 4, static_cast<sqlite3_int64>(batch_limit));

                struct PruneKey { std::string block_hash; std::int64_t log_index; };
                std::vector<PruneKey> keys;
                keys.reserve(batch_limit);

                int rc = SQLITE_OK;
                while ((rc = select_keys.step()) == SQLITE_ROW)
                {
                    PruneKey key{};
                    key.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(select_keys.get(), 0));
                    key.log_index = static_cast<std::int64_t>(sqlite3_column_int64(select_keys.get(), 1));
                    keys.push_back(std::move(key));
                }
                if (rc != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
                sqlite3_reset(select_keys.get());
                sqlite3_clear_bindings(select_keys.get());

                if (keys.empty())
                {
                    break;
                }

                if (!storage::sqlite::exec(_write_db, "BEGIN IMMEDIATE TRANSACTION;"))
                {
                    break;
                }

                try
                {
                    storage::sqlite::Statement del_norm(
                        _write_db,
                        "DELETE FROM normalized_events_hot WHERE source_id=?1 AND block_hash=?2 AND log_index=?3;");
                    storage::sqlite::Statement del_raw(
                        _write_db,
                        "DELETE FROM raw_events_hot WHERE source_id=?1 AND block_hash=?2 AND log_index=?3;");

                    for (const auto& key : keys)
                    {
                        sqlite3_bind_text(del_norm.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_text(
                            del_norm.get(), 2, key.block_hash.c_str(),
                            static_cast<int>(key.block_hash.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_int64(del_norm.get(), 3, static_cast<sqlite3_int64>(key.log_index));
                        if (del_norm.step() != SQLITE_DONE)
                        {
                            throw std::runtime_error(sqlite3_errmsg(_write_db));
                        }
                        sqlite3_reset(del_norm.get());
                        sqlite3_clear_bindings(del_norm.get());

                        sqlite3_bind_text(del_raw.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_text(
                            del_raw.get(), 2, key.block_hash.c_str(),
                            static_cast<int>(key.block_hash.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_int64(del_raw.get(), 3, static_cast<sqlite3_int64>(key.log_index));
                        if (del_raw.step() != SQLITE_DONE)
                        {
                            throw std::runtime_error(sqlite3_errmsg(_write_db));
                        }
                        sqlite3_reset(del_raw.get());
                        sqlite3_clear_bindings(del_raw.get());
                    }

                    if (!storage::sqlite::exec(_write_db, "COMMIT;"))
                    {
                        throw std::runtime_error("pruneConsumedRaw: commit failed");
                    }

                    total_pruned += keys.size();
                }
                catch (const std::exception& e)
                {
                    spdlog::error("Events pruneConsumedRaw batch failed: {}", e.what());
                    (void)storage::sqlite::exec(_write_db, "ROLLBACK;");
                    break;
                }

                if (keys.size() < batch_limit)
                {
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            spdlog::error("Events pruneConsumedRaw failed: {}", e.what());
        }

        if (total_pruned > 0)
        {
            spdlog::debug("Events pruneConsumedRaw: pruned={} events watermark={} floor_block={}",
                total_pruned, watermark, finalized_floor_block);
        }

        return total_pruned;
    }

    std::int64_t SQLiteHotStore::loadHeadBlock(const std::string & source_id)
    {
        storage::sqlite::Statement stmt(
            _write_db,
            "SELECT head_block FROM finality_state WHERE source_id=?1;");
        sqlite3_bind_text(stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
        if (stmt.step() == SQLITE_ROW)
        {
            return static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 0));
        }
        return 0;
    }

    FinalityCheckpoint SQLiteHotStore::loadFinalityCheckpoint(const std::string & source_id)
    {
        storage::sqlite::Statement stmt(
            _write_db,
            "SELECT finalized_block, finalized_hash FROM finality_state WHERE source_id=?1;");
        sqlite3_bind_text(stmt.get(), 1, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
        if (stmt.step() != SQLITE_ROW)
        {
            return FinalityCheckpoint{};
        }
        return FinalityCheckpoint{
            .finalized_block = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 0)),
            .finalized_hash = storage::sqlite::columnTextOptional(stmt.get(), 1).value_or("")};
    }

    bool SQLiteHotStore::_initializeHotSchema()
    {
        const bool schema_ok =
                storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS raw_events_hot ("
                    "source_id TEXT NOT NULL,"
                    "chain_namespace TEXT NOT NULL,"
                    "chain_id INTEGER NOT NULL,"
                    "block_hash TEXT NOT NULL,"
                    "log_index INTEGER NOT NULL,"
                    "tx_hash TEXT NOT NULL,"
                    "block_number INTEGER NOT NULL,"
                    "tx_index INTEGER NOT NULL,"
                    "block_time INTEGER,"
                    "address TEXT NOT NULL,"
                    "topic0 TEXT NOT NULL,"
                    "topic1 TEXT,"
                    "topic2 TEXT,"
                    "topic3 TEXT,"
                    "data_hex TEXT NOT NULL,"
                    "removed INTEGER NOT NULL DEFAULT 0,"
                    "state TEXT NOT NULL,"
                    "seen_at_ms INTEGER NOT NULL,"
                    "updated_at_ms INTEGER NOT NULL,"
                    "removed_at_ms INTEGER,"
                    "PRIMARY KEY(source_id, block_hash, log_index)"
                    ");") &&
                storage::sqlite::exec(_write_db,
                     "CREATE TABLE IF NOT EXISTS normalized_events_hot ("
                    "source_id TEXT NOT NULL,"
                    "chain_namespace TEXT NOT NULL,"
                    "chain_id INTEGER NOT NULL,"
                    "block_hash TEXT NOT NULL,"
                    "log_index INTEGER NOT NULL,"
                    "tx_hash TEXT NOT NULL,"
                    "block_number INTEGER NOT NULL,"
                    "tx_index INTEGER NOT NULL,"
                    "block_time INTEGER,"
                    "event_type TEXT NOT NULL,"
                    "name TEXT NOT NULL,"
                    "caller TEXT NOT NULL,"
                    "owner TEXT NOT NULL,"
                    "entity_address TEXT NOT NULL,"
                    "args_count INTEGER,"
                    "format_hash TEXT,"
                    "content_hash TEXT,"
                    "metadata_hash TEXT,"
                    "runtime_code_hash TEXT,"
                    "entity_kind TEXT,"
                    "old_owner TEXT,"
                    "state TEXT NOT NULL,"
                    "seen_at_ms INTEGER NOT NULL,"
                    "updated_at_ms INTEGER NOT NULL,"
                     "change_seq INTEGER NOT NULL DEFAULT 0,"
                     "dead_letter INTEGER NOT NULL DEFAULT 0,"
                     "PRIMARY KEY(source_id, block_hash, log_index)"
                     ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS decode_failures_hot ("
                    "source_id TEXT NOT NULL,"
                    "chain_id INTEGER NOT NULL,"
                    "block_hash TEXT NOT NULL,"
                    "log_index INTEGER NOT NULL,"
                    "last_error TEXT NOT NULL,"
                    "attempts INTEGER NOT NULL,"
                    "first_seen_at_ms INTEGER NOT NULL,"
                    "last_seen_at_ms INTEGER NOT NULL,"
                    "retryable INTEGER NOT NULL DEFAULT 1,"
                    "dead_letter INTEGER NOT NULL DEFAULT 0,"
                    "PRIMARY KEY(source_id, block_hash, log_index)"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS source_resume ("
                    "source_id TEXT PRIMARY KEY,"
                    "cursor_kind TEXT NOT NULL CHECK(cursor_kind IN ('local_sequence','next_block')),"
                    "next_cursor INTEGER NOT NULL,"
                    "updated_at_ms INTEGER NOT NULL"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS finality_state ("
                    "source_id TEXT PRIMARY KEY,"
                    "chain_id INTEGER NOT NULL,"
                    "head_block INTEGER NOT NULL,"
                    "safe_block INTEGER NOT NULL,"
                    "finalized_block INTEGER NOT NULL,"
                    "finalized_hash TEXT NOT NULL DEFAULT '',"
                    "updated_at_ms INTEGER NOT NULL"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS reorg_window ("
                    "source_id TEXT NOT NULL,"
                    "chain_id INTEGER NOT NULL,"
                    "block_number INTEGER NOT NULL,"
                    "block_hash TEXT NOT NULL,"
                    "parent_hash TEXT,"
                    "seen_at_ms INTEGER NOT NULL,"
                    "PRIMARY KEY(source_id, block_number)"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS change_seq_state ("
                    "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                    "next_seq INTEGER NOT NULL"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "INSERT OR IGNORE INTO change_seq_state(singleton, next_seq) VALUES(1, 1);") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_raw_source_tx_log ON raw_events_hot(source_id, tx_hash, log_index);") &&
               storage::sqlite::exec(_write_db, "CREATE INDEX IF NOT EXISTS idx_raw_source_state_block ON raw_events_hot(source_id, state, block_number);") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_norm_source_tx_log ON normalized_events_hot(source_id, tx_hash, "
                    "log_index);") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_norm_source_state_block ON normalized_events_hot(source_id, state, block_number);") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_decode_failures_retry "
                    "ON decode_failures_hot(source_id, dead_letter, retryable, last_seen_at_ms);") &&
               storage::sqlite::exec(_write_db,
                   "CREATE INDEX IF NOT EXISTS idx_norm_change_seq ON normalized_events_hot(change_seq);");

        if (!schema_ok)
        {
            return false;
        }

        if (!storage::sqlite::exec(
                _write_db,
                "CREATE INDEX IF NOT EXISTS idx_norm_dead_letter "
                "ON normalized_events_hot(change_seq) WHERE dead_letter<>0;"))
        {
            return false;
        }

        // Scratch set of the log keys one range scan returned, used by
        // _reconcileScannedRange. Connection-scoped, so it never touches the hot DB file.
        if (!storage::sqlite::exec(
                _write_db,
                "CREATE TEMP TABLE IF NOT EXISTS scan_generation("
                "block_hash TEXT NOT NULL, log_index INTEGER NOT NULL, "
                "PRIMARY KEY(block_hash, log_index));"))
        {
            return false;
        }

        return true;
    }

    std::int64_t SQLiteHotStore::_nextChangeSeq()
    {
        storage::sqlite::Statement select_stmt(
            _write_db, "SELECT next_seq FROM change_seq_state WHERE singleton=1;");
        if (select_stmt.step() != SQLITE_ROW)
        {
            throw std::runtime_error("change_seq_state: singleton row missing");
        }
        const std::int64_t seq = static_cast<std::int64_t>(sqlite3_column_int64(select_stmt.get(), 0));

        storage::sqlite::Statement update_stmt(
            _write_db, "UPDATE change_seq_state SET next_seq=next_seq+1 WHERE singleton=1;");
        if (update_stmt.step() != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_write_db));
        }
        return seq;
    }

    std::vector<ChangeRecord> SQLiteHotStore::readChangesSince(
        std::int64_t after_change_seq,
        std::size_t limit) const
    {
        storage::sqlite::Statement stmt(
            _read_db,
            std::format(
                "SELECT {}"
                "FROM normalized_events_hot n "
                "JOIN raw_events_hot r ON r.source_id=n.source_id AND r.block_hash=n.block_hash AND r.log_index=n.log_index "
                "WHERE n.change_seq > ?1 ORDER BY n.change_seq ASC LIMIT ?2;",
                CHANGE_RECORD_COLUMNS).c_str());

        sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(after_change_seq));
        sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(limit));

        std::vector<ChangeRecord> records;
        records.reserve(limit);

        int rc = SQLITE_OK;
        while ((rc = stmt.step()) == SQLITE_ROW)
        {
            records.push_back(_changeRecordFromRow(stmt.get()));
        }
        if (rc != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_read_db));
        }

        return records;
    }

    std::vector<ChangeRecord> SQLiteHotStore::readDeadLetters(
        const int projector_bit,
        const std::size_t limit) const
    {
        // The dead_letter<>0 predicate lets the partial index idx_norm_dead_letter
        // serve this query, so the sweep never scans the full hot table.
        storage::sqlite::Statement stmt(
            _read_db,
            std::format(
                "SELECT {}"
                "FROM normalized_events_hot n "
                "JOIN raw_events_hot r ON r.source_id=n.source_id AND r.block_hash=n.block_hash AND r.log_index=n.log_index "
                "WHERE n.dead_letter<>0 AND (n.dead_letter & ?1)<>0 "
                "ORDER BY n.change_seq ASC LIMIT ?2;",
                CHANGE_RECORD_COLUMNS).c_str());

        sqlite3_bind_int(stmt.get(), 1, projector_bit);
        sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(limit));

        std::vector<ChangeRecord> records;

        int rc = SQLITE_OK;
        while ((rc = stmt.step()) == SQLITE_ROW)
        {
            records.push_back(_changeRecordFromRow(stmt.get()));
        }
        if (rc != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_read_db));
        }

        return records;
    }

    bool SQLiteHotStore::markDeadLetter(
        const int projector_bit,
        const std::string & source_id,
        const std::string & block_hash,
        const std::int64_t log_index)
    {
        try
        {
            storage::sqlite::Statement stmt(
                _write_db,
                "UPDATE normalized_events_hot SET dead_letter = dead_letter | ?1 "
                "WHERE source_id=?2 AND block_hash=?3 AND log_index=?4;");

            sqlite3_bind_int(stmt.get(), 1, projector_bit);
            sqlite3_bind_text(stmt.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 3, block_hash.c_str(), static_cast<int>(block_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(log_index));

            if (stmt.step() != SQLITE_DONE)
            {
                spdlog::error("Events markDeadLetter failed: {}", sqlite3_errmsg(_write_db));
                return false;
            }

            // Zero changed rows means the key does not exist; the caller must not
            // advance past a row it could not quarantine.
            return sqlite3_changes(_write_db) > 0;
        }
        catch (const std::exception & e)
        {
            spdlog::error("Events markDeadLetter failed: {}", e.what());
            return false;
        }
    }

    bool SQLiteHotStore::clearDeadLetter(
        const int projector_bit,
        const std::string & source_id,
        const std::string & block_hash,
        const std::int64_t log_index)
    {
        try
        {
            storage::sqlite::Statement stmt(
                _write_db,
                "UPDATE normalized_events_hot SET dead_letter = dead_letter & ~?1 "
                "WHERE source_id=?2 AND block_hash=?3 AND log_index=?4;");

            sqlite3_bind_int(stmt.get(), 1, projector_bit);
            sqlite3_bind_text(stmt.get(), 2, source_id.c_str(), static_cast<int>(source_id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 3, block_hash.c_str(), static_cast<int>(block_hash.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(log_index));

            if (stmt.step() != SQLITE_DONE)
            {
                spdlog::error("Events clearDeadLetter failed: {}", sqlite3_errmsg(_write_db));
                return false;
            }

            return true;
        }
        catch (const std::exception & e)
        {
            spdlog::error("Events clearDeadLetter failed: {}", e.what());
            return false;
        }
    }

} // namespace dcn::events
