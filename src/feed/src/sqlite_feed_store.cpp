#include <algorithm>
#include <chrono>
#include <ranges>
#include <string_view>
#include <format>

#include <spdlog/spdlog.h>

#include "utils.hpp"
#include "sqlite/statement.hpp"
#include "sqlite/exec.hpp"

#include "decoded_event.hpp"
#include "sqlite_feed_store.hpp"

namespace dcn::feed
{
    constexpr int CURRENT_PROJECTOR_VERSION = 1;
    // Bumped whenever a feed or archive table definition changes; a database of another
    // version is refused.
    constexpr int FEED_SCHEMA_VERSION = 1;

    static bool _usesLogicalFeedIdentity(const std::string_view event_type)
    {
        return event_type == events::CONNECTOR_ADDED_TYPE
            || event_type == events::TRANSFORMATION_ADDED_TYPE
            || event_type == events::CONDITION_ADDED_TYPE;
    }

    static std::string _logicalFeedId(
        const std::string & chain_namespace,
        const std::uint64_t chain_id,
        const std::string & event_type,
        const std::string & name)
    {
        return std::format("{}:{}:{}:{}", chain_namespace, chain_id, event_type, name);
    }

    static std::string _projectedFeedId(
        const std::string & chain_namespace,
        const std::uint64_t chain_id,
        const std::string & tx_hash,
        const std::int64_t log_index,
        const std::string & event_type,
        const std::string & name)
    {
        if(_usesLogicalFeedIdentity(event_type))
        {
            return _logicalFeedId(chain_namespace, chain_id, event_type, name);
        }

        return std::format("{}:{}:{}:{}", chain_namespace, chain_id, tx_hash, log_index);
    }

    static std::string _compactFeedPayloadJson(
        const std::string& event_type,
        const std::string& name,
        const std::string& owner)
    {
        std::string type;

        if (event_type == events::toString(events::EventType::CONNECTOR_ADDED))
        {
            type = "connector";
        }
        else if (event_type == events::toString(events::EventType::TRANSFORMATION_ADDED))
        {
            type = "transformation";
        }
        else if (event_type == events::toString(events::EventType::CONDITION_ADDED))
        {
            type = "condition";
        }
        else
        {
            type = event_type;
        }

        json payload{
            {"type", type},
            {"name", name},
            {"owner", owner}
        };

        return payload.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    static std::optional<std::int64_t> _columnInt64Optional(sqlite3_stmt * stmt, const int index)
    {
        if(sqlite3_column_type(stmt, index) == SQLITE_NULL)
        {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt, index));
    }

    static std::optional<std::string> _columnTextOptional(sqlite3_stmt * stmt, const int index)
    {
        const unsigned char * txt = sqlite3_column_text(stmt, index);
        if(txt == nullptr)
        {
            return std::nullopt;
        }
        return std::string(reinterpret_cast<const char *>(txt));
    }

    static bool _feedDescComparator(const FeedItem & lhs, const FeedItem & rhs)
    {
        if(lhs.created_at_ms != rhs.created_at_ms)
        {
            return lhs.created_at_ms > rhs.created_at_ms;
        }
        if(lhs.block_number != rhs.block_number)
        {
            return lhs.block_number > rhs.block_number;
        }
        if(lhs.tx_index != rhs.tx_index)
        {
            return lhs.tx_index > rhs.tx_index;
        }
        return lhs.feed_id > rhs.feed_id;
    }

    static bool _cursorLessInDescOrder(const FeedItem & lhs, const CursorKey & rhs)
    {
        if(lhs.created_at_ms != rhs.created_at_ms)
        {
            return lhs.created_at_ms < rhs.created_at_ms;
        }
        if(lhs.block_number != rhs.block_number)
        {
            return lhs.block_number < rhs.block_number;
        }
        if(lhs.tx_index != rhs.tx_index)
        {
            return lhs.tx_index < rhs.tx_index;
        }
        return lhs.feed_id < rhs.feed_id;
    }

    struct FeedArchiveRow
    {
        std::string feed_id;
        std::string source_id;
        std::string chain_namespace;
        std::uint64_t chain_id = 1;
        std::string tx_hash;
        std::int64_t log_index = 0;
        std::int64_t block_number = 0;
        std::int64_t tx_index = 0;
        std::optional<std::int64_t> block_time = std::nullopt;
        std::string event_type;
        std::string status;
        bool visible = true;
        std::string history_cursor;
        std::string payload_json;
        std::int64_t created_at_ms = 0;
        std::int64_t updated_at_ms = 0;
        int projector_version = 1;
    };

    struct FeedHotKey
    {
        std::uint64_t chain_id = 1;
        std::string feed_id;
        int projector_version = 0;
        std::int64_t updated_at_ms = 0;
        std::string history_cursor;
        std::string payload_json;
    };

    struct MonthBounds
    {
        int year = 0;
        int month = 0;
        std::int64_t start_block_time = 0;
        std::int64_t end_block_time = 0;
    };

    static std::optional<MonthBounds> parseMonthBounds(const std::string& month_token)
    {
        if (month_token.size() != 7 || month_token[4] != '-')
        {
            return std::nullopt;
        }

        int year = 0;
        int month = 0;
        try
        {
            year = std::stoi(month_token.substr(0, 4));
            month = std::stoi(month_token.substr(5, 2));
        }
        catch (...)
        {
            return std::nullopt;
        }

        const std::chrono::year chrono_year{year};
        const std::chrono::month chrono_month{static_cast<unsigned>(month)};
        const std::chrono::year_month_day start_ymd{chrono_year, chrono_month, std::chrono::day{1}};
        if (!start_ymd.ok())
        {
            return std::nullopt;
        }

        const std::chrono::sys_days start_day{start_ymd};
        const std::chrono::sys_days end_day{start_ymd + std::chrono::months{1}};

        MonthBounds bounds{};
        bounds.year = year;
        bounds.month = month;
        bounds.start_block_time = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(start_day.time_since_epoch()).count());
        bounds.end_block_time = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(end_day.time_since_epoch()).count());
        return bounds;
    }

    static int _bindOptionalInt64(sqlite3_stmt * stmt, int index, const std::optional<std::int64_t> & value)
    {
        if(!value.has_value())
        {
            return sqlite3_bind_null(stmt, index);
        }
        return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(*value));
    }

    // -----------------------------------------------------------------------
    // Constructor / Destructor
    // -----------------------------------------------------------------------

    SqliteFeedStore::SqliteFeedStore(const std::filesystem::path& feed_db_path,
                                     const std::filesystem::path& archive_root,
                                     const std::int64_t outbox_retention_ms)
        : _feed_db_path(feed_db_path)
        , _archive_root(archive_root)
        , _outbox_retention_ms(outbox_retention_ms)
        , _shard_router(std::make_unique<events::MonthlyEventShardRouter>(_archive_root))
    {
        if (!_feed_db_path.parent_path().empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(_feed_db_path.parent_path(), ec);
            if (ec)
            {
                throw std::runtime_error(std::format(
                    "Failed to create feed DB directory '{}': {}", _feed_db_path.parent_path().string(), ec.message()));
            }
        }

        if (!_archive_root.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(_archive_root, ec);
            if (ec)
            {
                throw std::runtime_error(std::format(
                    "Failed to create feed archive directory '{}': {}", _archive_root.string(), ec.message()));
            }
        }

        if (utils::likelyNetworkPath(_feed_db_path))
        {
            throw std::runtime_error(std::format("Refusing to open feed DB on likely network filesystem path '{}'",
                                                 _feed_db_path.string()));
        }

        const int write_rc = sqlite3_open_v2(_feed_db_path.string().c_str(),
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

        const int read_rc = sqlite3_open_v2(_feed_db_path.string().c_str(),
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
            throw std::runtime_error("Failed to configure feed write DB pragmas");
        }

        if (!storage::sqlite::exec(_read_db, "PRAGMA journal_mode=WAL;")
            || !storage::sqlite::exec(_read_db, "PRAGMA synchronous=NORMAL;")
            || !storage::sqlite::exec(_read_db, "PRAGMA temp_store=MEMORY;")
            || !storage::sqlite::exec(_read_db, "PRAGMA foreign_keys=OFF;")
            || !storage::sqlite::exec(_read_db, "PRAGMA busy_timeout=10000;")
            || !storage::sqlite::exec(_read_db, "PRAGMA query_only=ON;"))
        {
            throw std::runtime_error("Failed to configure feed read DB pragmas");
        }

        if (!storage::sqlite::ensureSchemaVersion(_write_db, FEED_SCHEMA_VERSION, "feed")
            || !_initializeFeedSchema())
        {
            throw std::runtime_error("Failed to initialize feed DB schema");
        }
    }

    SqliteFeedStore::~SqliteFeedStore()
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

    // -----------------------------------------------------------------------
    // checkpointWal
    // -----------------------------------------------------------------------

    storage::sqlite::WalCheckpointStats SqliteFeedStore::checkpointWal(storage::sqlite::WalCheckpointMode mode)
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
            spdlog::warn("Feed WAL checkpoint mode={} failed during prepare/step: {}", mode_str, e.what());
            return stats;
        }

        std::error_code ec;
        const std::filesystem::path wal_path = std::filesystem::path(_feed_db_path.string() + "-wal");
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
                "Feed WAL checkpoint mode={} busy={} log_frames={} checkpointed_frames={} wal_bytes={}",
                mode_str,
                stats.busy,
                stats.log_frames,
                stats.checkpointed_frames,
                stats.wal_bytes);
        }
        else
        {
            spdlog::warn("Feed WAL checkpoint mode={} failed", mode_str);
        }

        return stats;
    }

    // -----------------------------------------------------------------------
    // runArchiveCycle
    // -----------------------------------------------------------------------

    bool SqliteFeedStore::runArchiveCycle(const std::size_t hot_window_days, const std::int64_t now_ms)
    {
        try
        {
            // Every chain the feed holds is archived, one shard per chain and month.
            std::vector<std::pair<std::uint64_t, std::string>> shards;
            {
                storage::sqlite::Statement shards_stmt(
                    _write_db,
                    "SELECT DISTINCT chain_id, strftime('%Y-%m', block_time, 'unixepoch') AS month_token "
                    "FROM feed_items_hot f "
                    "WHERE f.status='finalized' AND f.exported=0 "
                    "AND f.block_time IS NOT NULL AND f.projector_version>=?1 "
                    "ORDER BY month_token ASC, chain_id ASC "
                    "LIMIT 8;");

                sqlite3_bind_int(shards_stmt.get(), 1, CURRENT_PROJECTOR_VERSION);

                int shards_rc = SQLITE_OK;
                while ((shards_rc = shards_stmt.step()) == SQLITE_ROW)
                {
                    const unsigned char* month = sqlite3_column_text(shards_stmt.get(), 1);
                    if (month != nullptr)
                    {
                        shards.emplace_back(
                            static_cast<std::uint64_t>(sqlite3_column_int64(shards_stmt.get(), 0)),
                            reinterpret_cast<const char*>(month));
                    }
                }
                if (shards_rc != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
            }

            for (const auto& [chain_id, month] : shards)
            {
                if (!_exportMonth(chain_id, month, now_ms))
                {
                    return false;
                }
            }

            const std::int64_t cutoff_seconds =
                static_cast<std::int64_t>(now_ms / 1000) -
                static_cast<std::int64_t>(hot_window_days) * 24 * 60 * 60;

            {
                if (!storage::sqlite::exec(_write_db, "BEGIN IMMEDIATE TRANSACTION;"))
                {
                    return false;
                }

                try
                {
                    if (!storage::sqlite::exec(_write_db, "DROP TABLE IF EXISTS temp.prune_keys;"))
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }

                    // Build prune_keys from feed_items_hot (normalized/raw are pruned by the prune loop)
                    storage::sqlite::Statement create_prune_keys(
                        _write_db,
                        "CREATE TEMP TABLE prune_keys AS "
                        "SELECT chain_id, tx_hash, log_index "
                        "FROM feed_items_hot "
                        "WHERE status='finalized' AND exported=1 "
                        "AND block_time IS NOT NULL AND block_time < ?1;");

                    sqlite3_bind_int64(create_prune_keys.get(), 1, static_cast<sqlite3_int64>(cutoff_seconds));

                    if (create_prune_keys.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }

                    if (!storage::sqlite::exec(
                            _write_db,
                            "CREATE INDEX IF NOT EXISTS temp.idx_prune_keys_tx_log "
                            "ON prune_keys(chain_id, tx_hash, log_index);"))
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }

                    // Delete exported, old-enough feed items from hot (archived copies remain in shards)
                    storage::sqlite::Statement prune_feed(
                        _write_db,
                        "DELETE FROM feed_items_hot "
                        "WHERE exported=1 "
                        "AND EXISTS ("
                        "SELECT 1 FROM temp.prune_keys k "
                        "WHERE k.chain_id=feed_items_hot.chain_id "
                        "AND k.tx_hash=feed_items_hot.tx_hash "
                        "AND k.log_index=feed_items_hot.log_index"
                        ");");

                    if (prune_feed.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }

                    if (!storage::sqlite::exec(_write_db, "DROP TABLE IF EXISTS temp.prune_keys;"))
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
                    spdlog::error("Feed runArchiveCycle prune phase failed: {}", e.what());
                    (void)storage::sqlite::exec(_write_db, "ROLLBACK;");
                    return false;
                }
            }
        }
        catch (const std::exception& e)
        {
            spdlog::error("Feed runArchiveCycle failed: {}", e.what());
            return false;
        }
    }

    // -----------------------------------------------------------------------
    // getFeedPage
    // -----------------------------------------------------------------------

    FeedPage SqliteFeedStore::getFeedPage(const FeedQuery& query) const
    {
        const std::size_t limit =
            std::clamp<std::size_t>((query.limit == 0) ? DEFAULT_FEED_LIMIT : query.limit, 1, MAX_FEED_LIMIT);

        parse::Result<CursorKey> before_key_res = {};
        std::optional<CursorKey> before_key = std::nullopt;

        if (query.before_cursor.has_value())
        {
            before_key_res = parseHistoryCursor(*query.before_cursor);
            if (!before_key_res.has_value())
            {
                spdlog::error("Failed to parse before cursor: {}", *query.before_cursor);
                return FeedPage {};
            }
            else
            {
                before_key = before_key_res.value();
            }
        }

        std::vector<FeedItem> all_items;
        all_items.reserve(limit + 16);

        std::unordered_set<std::string> seen_feed_ids;

        _appendFeedRowsFromDatabase(_read_db, "feed_items_hot", query, before_key, limit + 1, all_items, seen_feed_ids);

        // Feed order is by created_at_ms (projection time), which is not aligned
        // with archival (driven by block-time aging), so an archived row can
        // outrank a full hot page. Always scan the archive shards and let the
        // sort/truncate below pick the true top rows.
        const std::vector<std::filesystem::path> archive_paths = _candidateArchivePaths();

        for (const auto& archive_path : archive_paths)
        {
            sqlite3* archive_db = nullptr;
            const int open_rc = sqlite3_open_v2(
                archive_path.string().c_str(),
                &archive_db,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                nullptr);

            if (open_rc != SQLITE_OK)
            {
                if (archive_db != nullptr)
                {
                    sqlite3_close(archive_db);
                }
                continue;
            }

            sqlite3_busy_timeout(archive_db, 10'000);

            _appendFeedRowsFromDatabase(
                archive_db,
                "feed_items_archive",
                query,
                before_key,
                limit + 1,
                all_items,
                seen_feed_ids);

            sqlite3_close(archive_db);
            archive_db = nullptr;
        }

        std::ranges::sort(all_items, _feedDescComparator);

        FeedPage page {};
        page.has_more = all_items.size() > limit;

        if (page.has_more)
        {
            all_items.resize(limit);
        }

        page.items = std::move(all_items);

        if (page.has_more && !page.items.empty())
        {
            page.next_before_cursor = page.items.back().history_cursor;
        }

        return page;
    }

    // -----------------------------------------------------------------------
    // getStreamPage
    // -----------------------------------------------------------------------

    StreamPage SqliteFeedStore::getStreamPage(const StreamQuery& query) const
    {
        const std::size_t limit =
            std::clamp<std::size_t>((query.limit == 0) ? DEFAULT_STREAM_LIMIT : query.limit, 1, MAX_STREAM_LIMIT);
        StreamPage page {};

        {
            storage::sqlite::Statement replay_floor_stmt(
                _read_db,
                "SELECT replay_floor_seq FROM outbox_stream_state WHERE singleton=1;");
            const int replay_floor_rc = replay_floor_stmt.step();
            if (replay_floor_rc == SQLITE_ROW)
            {
                page.replay_floor_seq = static_cast<std::int64_t>(sqlite3_column_int64(replay_floor_stmt.get(), 0));
            }
            else if (replay_floor_rc != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_read_db));
            }

            storage::sqlite::Statement min_seq_stmt(_read_db, "SELECT MIN(stream_seq) FROM global_outbox;");
            std::int64_t min_stream_seq = 0;
            const int min_seq_rc = min_seq_stmt.step();
            if (min_seq_rc == SQLITE_ROW)
            {
                min_stream_seq = (sqlite3_column_type(min_seq_stmt.get(), 0) == SQLITE_NULL)
                    ? 0
                    : static_cast<std::int64_t>(sqlite3_column_int64(min_seq_stmt.get(), 0));
            }
            else if (min_seq_rc != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_read_db));
            }
            page.min_available_seq = std::max(page.replay_floor_seq, min_stream_seq);
        }

        page.stale_since_seq = query.since_seq > 0 && query.since_seq < page.min_available_seq;

        storage::sqlite::Statement stream_stmt(
            _read_db,
            "SELECT "
            "o.stream_seq, o.status, o.feed_id, o.history_cursor, o.payload_json, o.created_at_ms, o.event_type "
            "FROM global_outbox o "
            "WHERE o.stream_seq > ?1 "
            "ORDER BY o.stream_seq ASC "
            "LIMIT ?2;");
        sqlite3_bind_int64(stream_stmt.get(), 1, static_cast<sqlite3_int64>(query.since_seq));
        sqlite3_bind_int64(stream_stmt.get(), 2, static_cast<sqlite3_int64>(limit + 1));

        int stream_rc = SQLITE_OK;
        while ((stream_rc = stream_stmt.step()) == SQLITE_ROW)
        {
            StreamDelta delta {};
            delta.stream_seq = static_cast<std::int64_t>(sqlite3_column_int64(stream_stmt.get(), 0));
            delta.status = reinterpret_cast<const char*>(sqlite3_column_text(stream_stmt.get(), 1));
            delta.feed_id = reinterpret_cast<const char*>(sqlite3_column_text(stream_stmt.get(), 2));
            delta.history_cursor = reinterpret_cast<const char*>(sqlite3_column_text(stream_stmt.get(), 3));
            const std::string payload_json = reinterpret_cast<const char*>(sqlite3_column_text(stream_stmt.get(), 4));
            delta.created_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stream_stmt.get(), 5));
            const unsigned char * event_type_txt = sqlite3_column_text(stream_stmt.get(), 6);
            delta.event_type = (event_type_txt == nullptr)
                ? std::string{}
                : std::string(reinterpret_cast<const char *>(event_type_txt));
            delta.payload = json::parse(payload_json, nullptr, false);
            if (delta.payload.is_discarded())
            {
                delta.payload = json::object();
            }
            page.deltas.push_back(std::move(delta));
        }
        if (stream_rc != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_read_db));
        }

        page.has_more = page.deltas.size() > limit;
        if (page.has_more)
        {
            page.deltas.resize(limit);
        }
        if (!page.deltas.empty())
        {
            page.last_seq = page.deltas.back().stream_seq;
        }
        return page;
    }

    // -----------------------------------------------------------------------
    // minAvailableStreamSeq
    // -----------------------------------------------------------------------

    std::int64_t SqliteFeedStore::minAvailableStreamSeq() const
    {
        std::int64_t replay_floor_seq = 0;
        {
            storage::sqlite::Statement floor_stmt(_read_db, "SELECT replay_floor_seq FROM outbox_stream_state WHERE singleton=1;");
            const int floor_rc = floor_stmt.step();
            if (floor_rc == SQLITE_ROW)
            {
                replay_floor_seq = static_cast<std::int64_t>(sqlite3_column_int64(floor_stmt.get(), 0));
            }
            else if (floor_rc != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_read_db));
            }
        }
        storage::sqlite::Statement min_stmt(_read_db, "SELECT MIN(stream_seq) FROM global_outbox;");
        const int min_rc = min_stmt.step();
        if (min_rc == SQLITE_ROW)
        {
            const std::int64_t min_stream_seq = (sqlite3_column_type(min_stmt.get(), 0) == SQLITE_NULL)
                ? 0
                : static_cast<std::int64_t>(sqlite3_column_int64(min_stmt.get(), 0));
            return std::max(replay_floor_seq, min_stream_seq);
        }
        if (min_rc != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_read_db));
        }
        return replay_floor_seq;
    }

    // -----------------------------------------------------------------------
    // _initializeFeedSchema
    // -----------------------------------------------------------------------

    bool SqliteFeedStore::_initializeFeedSchema()
    {
        const bool schema_ok =
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS feed_items_hot ("
                    "feed_id TEXT PRIMARY KEY,"
                    "source_id TEXT NOT NULL DEFAULT 'local',"
                    "chain_namespace TEXT NOT NULL DEFAULT 'local',"
                    "chain_id INTEGER NOT NULL,"
                    "tx_hash TEXT NOT NULL,"
                    "log_index INTEGER NOT NULL,"
                    "block_number INTEGER NOT NULL,"
                    "tx_index INTEGER NOT NULL,"
                    "block_time INTEGER,"
                    "event_type TEXT NOT NULL,"
                    "status TEXT NOT NULL,"
                    "visible INTEGER NOT NULL,"
                    "history_cursor TEXT NOT NULL,"
                    "payload_json TEXT NOT NULL,"
                    "created_at_ms INTEGER NOT NULL,"
                    "updated_at_ms INTEGER NOT NULL,"
                    "projector_version INTEGER NOT NULL,"
                    "exported INTEGER NOT NULL DEFAULT 0,"
                    "stream_emitted INTEGER NOT NULL DEFAULT 0"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS global_outbox ("
                    "stream_seq INTEGER PRIMARY KEY,"
                    "source_id TEXT NOT NULL DEFAULT 'local',"
                    "chain_namespace TEXT NOT NULL DEFAULT 'local',"
                    "feed_id TEXT NOT NULL,"
                    "op TEXT NOT NULL,"
                    "status TEXT NOT NULL,"
                    "event_type TEXT NOT NULL,"
                    "history_cursor TEXT NOT NULL,"
                    "payload_json TEXT NOT NULL,"
                    "created_at_ms INTEGER NOT NULL"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS outbox_stream_state ("
                    "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                    "next_stream_seq INTEGER NOT NULL,"
                    "replay_floor_seq INTEGER NOT NULL DEFAULT 0"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "INSERT OR IGNORE INTO outbox_stream_state(singleton, next_stream_seq, replay_floor_seq) "
                    "VALUES(1, "
                    "COALESCE((SELECT MAX(stream_seq) + 1 FROM global_outbox), 1), "
                    "COALESCE((SELECT MIN(stream_seq) FROM global_outbox), 0)"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "UPDATE outbox_stream_state "
                    "SET next_stream_seq=MAX(next_stream_seq, COALESCE((SELECT MAX(stream_seq) + 1 FROM global_outbox), 1)), "
                    "replay_floor_seq=MAX(replay_floor_seq, COALESCE((SELECT MIN(stream_seq) FROM global_outbox), 0)) "
                    "WHERE singleton=1;") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS feed_cursor ("
                    "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                    "last_change_seq INTEGER NOT NULL"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "INSERT OR IGNORE INTO feed_cursor(singleton, last_change_seq) VALUES(1, 0);") &&
               storage::sqlite::exec(_write_db,
                    "CREATE TABLE IF NOT EXISTS shard_catalog ("
                    "chain_id INTEGER NOT NULL,"
                    "archive_month TEXT NOT NULL,"
                    "path TEXT NOT NULL,"
                    "state TEXT NOT NULL,"
                    "min_block INTEGER NOT NULL,"
                    "max_block INTEGER NOT NULL,"
                    "row_count INTEGER NOT NULL,"
                    "last_export_ms INTEGER NOT NULL,"
                    "PRIMARY KEY(chain_id, archive_month)"
                    ");") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_visible_order ON feed_items_hot(block_number DESC, tx_index DESC, "
                    "log_index DESC, feed_id DESC) WHERE visible=1;") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_chain_visible_order "
                    "ON feed_items_hot(chain_id, block_number DESC, tx_index DESC, log_index DESC, feed_id DESC) "
                    "WHERE visible=1;") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_event_type_order ON feed_items_hot(event_type, block_number DESC, "
                    "tx_index DESC, log_index DESC, feed_id DESC);") &&
                storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_visible_updated_order "
                    "ON feed_items_hot(created_at_ms DESC, block_number DESC, tx_index DESC, feed_id DESC) "
                    "WHERE visible=1;") &&
                storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_chain_visible_updated_order "
                    "ON feed_items_hot(chain_id, created_at_ms DESC, block_number DESC, tx_index DESC, "
                    "feed_id DESC) WHERE visible=1;") &&
                storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_event_type_updated_order "
                    "ON feed_items_hot(event_type, created_at_ms DESC, block_number DESC, tx_index DESC, "
                    "feed_id DESC);") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_chain_tx_log "
                    "ON feed_items_hot(chain_id, tx_hash, log_index);") &&
               storage::sqlite::exec(_write_db,
                    "CREATE INDEX IF NOT EXISTS idx_feed_archive_ready "
                    "ON feed_items_hot(chain_id, status, exported, block_time);") &&
               storage::sqlite::exec(_write_db, "CREATE INDEX IF NOT EXISTS idx_outbox_feed_id ON global_outbox(feed_id);") &&
               storage::sqlite::exec(_write_db, "CREATE INDEX IF NOT EXISTS idx_outbox_created_at ON global_outbox(created_at_ms);") &&
               storage::sqlite::exec(
                   _write_db,
                   "CREATE INDEX IF NOT EXISTS idx_catalog_state_block ON shard_catalog(chain_id, state, max_block DESC);");

        if (!schema_ok)
        {
            return false;
        }

        return true;
    }

    // -----------------------------------------------------------------------
    // _initializeArchiveSchema
    // -----------------------------------------------------------------------

    bool SqliteFeedStore::_initializeArchiveSchema(sqlite3* archive_db) const
    {
        return storage::sqlite::exec(archive_db,
                    "CREATE TABLE IF NOT EXISTS feed_items_archive ("
                    "feed_id TEXT PRIMARY KEY,"
                    "source_id TEXT NOT NULL DEFAULT 'local',"
                    "chain_namespace TEXT NOT NULL DEFAULT 'local',"
                    "chain_id INTEGER NOT NULL,"
                    "tx_hash TEXT NOT NULL,"
                    "log_index INTEGER NOT NULL,"
                    "block_number INTEGER NOT NULL,"
                    "tx_index INTEGER NOT NULL,"
                    "block_time INTEGER,"
                    "event_type TEXT NOT NULL,"
                    "status TEXT NOT NULL,"
                    "visible INTEGER NOT NULL,"
                    "history_cursor TEXT NOT NULL,"
                    "payload_json TEXT NOT NULL,"
                    "created_at_ms INTEGER NOT NULL,"
                    "updated_at_ms INTEGER NOT NULL,"
                    "projector_version INTEGER NOT NULL"
                    ");") &&
               storage::sqlite::exec(archive_db,
                    "CREATE INDEX IF NOT EXISTS idx_archive_feed_visible_order ON feed_items_archive(block_number DESC, "
                    "tx_index DESC, log_index DESC, feed_id DESC) WHERE visible=1;") &&
               storage::sqlite::exec(archive_db,
                    "CREATE INDEX IF NOT EXISTS idx_archive_feed_chain_visible_order "
                    "ON feed_items_archive(chain_id, block_number DESC, tx_index DESC, log_index DESC, feed_id DESC) "
                    "WHERE visible=1;") &&
               storage::sqlite::exec(archive_db,
                    "CREATE INDEX IF NOT EXISTS idx_archive_feed_type_order ON feed_items_archive(event_type, block_number "
                    "DESC, tx_index DESC, log_index DESC, feed_id DESC);") &&
               storage::sqlite::exec(archive_db,
                    "CREATE INDEX IF NOT EXISTS idx_archive_feed_visible_updated_order "
                    "ON feed_items_archive(created_at_ms DESC, block_number DESC, tx_index DESC, "
                    "feed_id DESC) WHERE visible=1;") &&
               storage::sqlite::exec(archive_db,
                    "CREATE INDEX IF NOT EXISTS idx_archive_feed_chain_visible_updated_order "
                    "ON feed_items_archive(chain_id, created_at_ms DESC, block_number DESC, tx_index DESC, "
                    "feed_id DESC) WHERE visible=1;") &&
               storage::sqlite::exec(archive_db,
                    "CREATE INDEX IF NOT EXISTS idx_archive_feed_type_updated_order "
                    "ON feed_items_archive(event_type, created_at_ms DESC, block_number DESC, tx_index DESC, "
                    "feed_id DESC);");
    }

    // -----------------------------------------------------------------------
    // _exportMonth
    // -----------------------------------------------------------------------

    bool SqliteFeedStore::_exportMonth(
        const std::uint64_t chain_id,
        const std::string& month_token,
        const std::int64_t now_ms)
    {
        const auto month_bounds = parseMonthBounds(month_token);
        if (!month_bounds.has_value())
        {
            return false;
        }

        events::EventShardId shard_id{.chain_id = chain_id, .year = month_bounds->year, .month = month_bounds->month};
        const std::filesystem::path archive_path = _shard_router->filenameFor(shard_id);

        // Only feed items are exported to the archive shard.
        std::vector<FeedArchiveRow> feed_rows;
        std::vector<FeedHotKey> feed_hot_keys;
        {
            {
                storage::sqlite::Statement select_feed(_write_db,
                                      "SELECT "
                                      "feed_id, chain_id, tx_hash, log_index, block_number, tx_index, block_time, "
                                      "event_type, status, visible, history_cursor, payload_json, "
                                      "created_at_ms, updated_at_ms, projector_version, source_id, chain_namespace "
                                      "FROM feed_items_hot f "
                                      "WHERE f.chain_id=?1 AND f.status='finalized' AND f.exported=0 "
                                      "AND f.block_time IS NOT NULL AND f.block_time>=?2 AND f.block_time<?3 "
                                      "AND f.projector_version>=?4;");

                sqlite3_bind_int64(select_feed.get(), 1, static_cast<sqlite3_int64>(chain_id));
                sqlite3_bind_int64(select_feed.get(), 2, static_cast<sqlite3_int64>(month_bounds->start_block_time));
                sqlite3_bind_int64(select_feed.get(), 3, static_cast<sqlite3_int64>(month_bounds->end_block_time));
                sqlite3_bind_int(select_feed.get(), 4, CURRENT_PROJECTOR_VERSION);

                int select_feed_rc = SQLITE_OK;
                while ((select_feed_rc = select_feed.step()) == SQLITE_ROW)
                {
                    FeedArchiveRow row{};
                    row.feed_id = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 0));
                    row.chain_id = static_cast<std::uint64_t>(sqlite3_column_int64(select_feed.get(), 1));
                    row.tx_hash = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 2));
                    row.log_index = static_cast<std::int64_t>(sqlite3_column_int64(select_feed.get(), 3));
                    row.block_number = static_cast<std::int64_t>(sqlite3_column_int64(select_feed.get(), 4));
                    row.tx_index = static_cast<std::int64_t>(sqlite3_column_int64(select_feed.get(), 5));
                    row.block_time = _columnInt64Optional(select_feed.get(), 6);
                    row.event_type = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 7));
                    row.status = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 8));
                    row.visible = sqlite3_column_int(select_feed.get(), 9) != 0;
                    row.history_cursor = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 10));
                    row.payload_json = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 11));
                    row.created_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(select_feed.get(), 12));
                    row.updated_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(select_feed.get(), 13));
                    row.projector_version = sqlite3_column_int(select_feed.get(), 14);
                    row.source_id = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 15));
                    row.chain_namespace = reinterpret_cast<const char*>(sqlite3_column_text(select_feed.get(), 16));
                    feed_hot_keys.push_back(FeedHotKey{
                        .chain_id = row.chain_id,
                        .feed_id = row.feed_id,
                        .projector_version = row.projector_version,
                        .updated_at_ms = row.updated_at_ms,
                        .history_cursor = row.history_cursor,
                        .payload_json = row.payload_json});
                    feed_rows.push_back(std::move(row));
                }
                if (select_feed_rc != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
            }
        }

        if (feed_rows.empty())
        {
            return true;
        }

        const bool archive_write_ok =
            [this,
             month_token,
             archive_path,
             feed_rows = std::move(feed_rows)]() mutable -> bool
            {
                std::error_code dir_ec;
                std::filesystem::create_directories(archive_path.parent_path(), dir_ec);
                if (dir_ec)
                {
                    spdlog::error(
                        "Failed to create archive shard directory '{}': {}",
                        archive_path.parent_path().string(),
                        dir_ec.message());
                    return false;
                }

                sqlite3* archive_db = nullptr;
                const int open_rc = sqlite3_open_v2(archive_path.string().c_str(),
                                                    &archive_db,
                                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                                    nullptr);
                if (open_rc != SQLITE_OK)
                {
                    const std::string err = (archive_db == nullptr) ? "sqlite open failed" : sqlite3_errmsg(archive_db);
                    if (archive_db != nullptr)
                    {
                        sqlite3_close(archive_db);
                        archive_db = nullptr;
                    }
                    spdlog::error("Failed to open archive shard '{}': {}", archive_path.string(), err);
                    return false;
                }

                sqlite3_busy_timeout(archive_db, 10'000);

                // Shards are one-shot single-transaction bulk writes with no concurrent
                // readers (a shard is not marked READY until after commit), so WAL buys
                // nothing and leaves -wal/-shm sidecars behind. Rollback journal keeps
                // each shard a single file at rest.
                if (!storage::sqlite::exec(archive_db, "PRAGMA journal_mode=DELETE;")
                    || !storage::sqlite::exec(archive_db, "PRAGMA synchronous=NORMAL;")
                    || !storage::sqlite::exec(archive_db, "PRAGMA temp_store=MEMORY;")
                    || !storage::sqlite::exec(archive_db, "PRAGMA foreign_keys=OFF;"))
                {
                    sqlite3_close(archive_db);
                    archive_db = nullptr;
                    return false;
                }

                if (!storage::sqlite::ensureSchemaVersion(archive_db, FEED_SCHEMA_VERSION, "feed archive shard")
                    || !_initializeArchiveSchema(archive_db))
                {
                    sqlite3_close(archive_db);
                    archive_db = nullptr;
                    return false;
                }

                if (!storage::sqlite::exec(archive_db, "BEGIN IMMEDIATE TRANSACTION;"))
                {
                    sqlite3_close(archive_db);
                    archive_db = nullptr;
                    return false;
                }

                try
                {
                    storage::sqlite::Statement insert_feed(archive_db,
                                          "INSERT INTO feed_items_archive("
                                          "feed_id, chain_id, tx_hash, log_index, block_number, tx_index, block_time, "
                                          "event_type, status, visible, history_cursor, payload_json, "
                                          "created_at_ms, updated_at_ms, projector_version, source_id, chain_namespace"
                                          ") VALUES("
                                          "?1, ?2, ?3, ?4, ?5, ?6, ?7, "
                                          "?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17"
                                          ") ON CONFLICT(feed_id) DO UPDATE SET "
                                          "chain_id=excluded.chain_id, tx_hash=excluded.tx_hash, log_index=excluded.log_index, "
                                          "block_number=excluded.block_number, tx_index=excluded.tx_index, "
                                          "block_time=excluded.block_time, event_type=excluded.event_type, status=excluded.status, "
                                          "visible=excluded.visible, history_cursor=excluded.history_cursor, "
                                          "source_id=excluded.source_id, chain_namespace=excluded.chain_namespace, "
                                          "payload_json=excluded.payload_json, "
                                          "created_at_ms=excluded.created_at_ms, updated_at_ms=excluded.updated_at_ms, "
                                          "projector_version=excluded.projector_version;");

                    for (const auto& row : feed_rows)
                    {
                        sqlite3_bind_text(
                            insert_feed.get(), 1, row.feed_id.c_str(), static_cast<int>(row.feed_id.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_int64(insert_feed.get(), 2, static_cast<sqlite3_int64>(row.chain_id));
                        sqlite3_bind_text(
                            insert_feed.get(), 3, row.tx_hash.c_str(), static_cast<int>(row.tx_hash.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_int64(insert_feed.get(), 4, static_cast<sqlite3_int64>(row.log_index));
                        sqlite3_bind_int64(insert_feed.get(), 5, static_cast<sqlite3_int64>(row.block_number));
                        sqlite3_bind_int64(insert_feed.get(), 6, static_cast<sqlite3_int64>(row.tx_index));
                        _bindOptionalInt64(insert_feed.get(), 7, row.block_time);
                        sqlite3_bind_text(insert_feed.get(),
                                          8,
                                          row.event_type.c_str(),
                                          static_cast<int>(row.event_type.size()),
                                          SQLITE_TRANSIENT);
                        sqlite3_bind_text(
                            insert_feed.get(), 9, row.status.c_str(), static_cast<int>(row.status.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_int(insert_feed.get(), 10, row.visible ? 1 : 0);
                        sqlite3_bind_text(insert_feed.get(),
                                          11,
                                          row.history_cursor.c_str(),
                                          static_cast<int>(row.history_cursor.size()),
                                          SQLITE_TRANSIENT);
                        sqlite3_bind_text(insert_feed.get(),
                                          12,
                                          row.payload_json.c_str(),
                                          static_cast<int>(row.payload_json.size()),
                                          SQLITE_TRANSIENT);
                        sqlite3_bind_int64(insert_feed.get(), 13, static_cast<sqlite3_int64>(row.created_at_ms));
                        sqlite3_bind_int64(insert_feed.get(), 14, static_cast<sqlite3_int64>(row.updated_at_ms));
                        sqlite3_bind_int(insert_feed.get(), 15, row.projector_version);
                        sqlite3_bind_text(insert_feed.get(), 16, row.source_id.c_str(), static_cast<int>(row.source_id.size()), SQLITE_TRANSIENT);
                        sqlite3_bind_text(insert_feed.get(), 17, row.chain_namespace.c_str(), static_cast<int>(row.chain_namespace.size()), SQLITE_TRANSIENT);
                        if (insert_feed.step() != SQLITE_DONE)
                        {
                            throw std::runtime_error(sqlite3_errmsg(archive_db));
                        }
                        sqlite3_reset(insert_feed.get());
                        sqlite3_clear_bindings(insert_feed.get());
                    }

                    if (!storage::sqlite::exec(archive_db, "COMMIT;"))
                    {
                        throw std::runtime_error("archive commit failed");
                    }
                }
                catch (const std::exception& e)
                {
                    spdlog::error("Archive export write failed for month {}: {}", month_token, e.what());
                    (void)storage::sqlite::exec(archive_db, "ROLLBACK;");
                    sqlite3_close(archive_db);
                    archive_db = nullptr;
                    return false;
                }

                sqlite3_close(archive_db);
                archive_db = nullptr;
                return true;
            }();

        if (!archive_write_ok)
        {
            return false;
        }

        {
            if (!storage::sqlite::exec(_write_db, "BEGIN IMMEDIATE TRANSACTION;"))
            {
                return false;
            }

            try
            {
                storage::sqlite::Statement mark_feed_exported(
                    _write_db,
                    "UPDATE feed_items_hot SET exported=1, updated_at_ms=?1 "
                    "WHERE feed_id=?2 AND chain_id=?3 AND status='finalized' "
                    "AND projector_version=?4 AND updated_at_ms=?5 "
                    "AND history_cursor=?6 AND payload_json=?7 AND exported=0;");

                std::size_t feed_snapshot_mismatch = 0;
                for (const auto& key : feed_hot_keys)
                {
                    sqlite3_bind_int64(mark_feed_exported.get(), 1, static_cast<sqlite3_int64>(now_ms));
                    sqlite3_bind_text(mark_feed_exported.get(),
                                      2,
                                      key.feed_id.c_str(),
                                      static_cast<int>(key.feed_id.size()),
                                      SQLITE_TRANSIENT);
                    sqlite3_bind_int64(mark_feed_exported.get(), 3, static_cast<sqlite3_int64>(key.chain_id));
                    sqlite3_bind_int(mark_feed_exported.get(), 4, key.projector_version);
                    sqlite3_bind_int64(mark_feed_exported.get(), 5, static_cast<sqlite3_int64>(key.updated_at_ms));
                    sqlite3_bind_text(mark_feed_exported.get(),
                                      6,
                                      key.history_cursor.c_str(),
                                      static_cast<int>(key.history_cursor.size()),
                                      SQLITE_TRANSIENT);
                    sqlite3_bind_text(mark_feed_exported.get(),
                                      7,
                                      key.payload_json.c_str(),
                                      static_cast<int>(key.payload_json.size()),
                                      SQLITE_TRANSIENT);
                    if (mark_feed_exported.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                    if (sqlite3_changes(_write_db) == 0)
                    {
                        ++feed_snapshot_mismatch;
                    }
                    sqlite3_reset(mark_feed_exported.get());
                    sqlite3_clear_bindings(mark_feed_exported.get());
                }

                if (feed_snapshot_mismatch > 0)
                {
                    spdlog::warn(
                        "Archive export finalization: {}/{} feed rows had snapshot mismatches for month {}; "
                        "leaving them unexported for next cycle",
                        feed_snapshot_mismatch,
                        feed_hot_keys.size(),
                        month_token);
                }

                std::int64_t min_block = 0;
                std::int64_t max_block = 0;
                std::int64_t row_count = 0;
                {
                    storage::sqlite::Statement stats_stmt(_write_db,
                                         "SELECT COALESCE(MIN(block_number), 0), COALESCE(MAX(block_number), 0), COUNT(1) "
                                         "FROM feed_items_hot "
                                         "WHERE chain_id=?1 AND status='finalized' AND projector_version>=?2 AND exported=1 "
                                         "AND block_time IS NOT NULL AND block_time>=?3 AND block_time<?4;");
                    sqlite3_bind_int64(stats_stmt.get(), 1, static_cast<sqlite3_int64>(chain_id));
                    sqlite3_bind_int(stats_stmt.get(), 2, CURRENT_PROJECTOR_VERSION);
                    sqlite3_bind_int64(stats_stmt.get(), 3, static_cast<sqlite3_int64>(month_bounds->start_block_time));
                    sqlite3_bind_int64(stats_stmt.get(), 4, static_cast<sqlite3_int64>(month_bounds->end_block_time));

                    const int stats_rc = stats_stmt.step();
                    if (stats_rc == SQLITE_ROW)
                    {
                        min_block = static_cast<std::int64_t>(sqlite3_column_int64(stats_stmt.get(), 0));
                        max_block = static_cast<std::int64_t>(sqlite3_column_int64(stats_stmt.get(), 1));
                        row_count = static_cast<std::int64_t>(sqlite3_column_int64(stats_stmt.get(), 2));
                    }
                    else if (stats_rc != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }

                storage::sqlite::Statement catalog_stmt(
                    _write_db,
                    "INSERT INTO shard_catalog(chain_id, archive_month, path, state, min_block, max_block, row_count, "
                    "last_export_ms) "
                    "VALUES(?1, ?2, ?3, 'READY', ?4, ?5, ?6, ?7) "
                    "ON CONFLICT(chain_id, archive_month) DO UPDATE SET "
                    "path=excluded.path, state='READY', min_block=excluded.min_block, max_block=excluded.max_block, "
                    "row_count=excluded.row_count, last_export_ms=excluded.last_export_ms;");

                sqlite3_bind_int64(catalog_stmt.get(), 1, static_cast<sqlite3_int64>(chain_id));
                sqlite3_bind_text(catalog_stmt.get(), 2, month_token.c_str(), static_cast<int>(month_token.size()), SQLITE_TRANSIENT);

                const std::string archive_path_str = archive_path.string();
                sqlite3_bind_text(catalog_stmt.get(),
                                  3,
                                  archive_path_str.c_str(),
                                  static_cast<int>(archive_path_str.size()),
                                  SQLITE_TRANSIENT);
                sqlite3_bind_int64(catalog_stmt.get(), 4, static_cast<sqlite3_int64>(min_block));
                sqlite3_bind_int64(catalog_stmt.get(), 5, static_cast<sqlite3_int64>(max_block));
                sqlite3_bind_int64(catalog_stmt.get(), 6, static_cast<sqlite3_int64>(row_count));
                sqlite3_bind_int64(catalog_stmt.get(), 7, static_cast<sqlite3_int64>(now_ms));

                if (catalog_stmt.step() != SQLITE_DONE)
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
                spdlog::error("Archive export finalization failed for month {}: {}", month_token, e.what());
                (void)storage::sqlite::exec(_write_db, "ROLLBACK;");
                return false;
            }
        }
    }

    // -----------------------------------------------------------------------
    // _candidateArchivePaths
    // -----------------------------------------------------------------------

    std::vector<std::filesystem::path> SqliteFeedStore::_candidateArchivePaths() const
    {
        std::vector<std::filesystem::path> paths;

        // Every chain's shards take part: the page order is by projection time, which
        // neither a chain nor a block range bounds, and block numbers of different
        // sources are not comparable.
        // ponytail: one open per READY shard per page; record created_at_ms bounds in the
        // catalog and skip shards past the cursor once shard counts make pages slow.
        storage::sqlite::Statement stmt(
            _read_db,
            "SELECT path FROM shard_catalog WHERE state='READY' ORDER BY archive_month DESC, chain_id ASC;");

        int rc = SQLITE_OK;
        while ((rc = stmt.step()) == SQLITE_ROW)
        {
            const unsigned char* txt = sqlite3_column_text(stmt.get(), 0);
            if (txt == nullptr)
            {
                continue;
            }
            paths.emplace_back(reinterpret_cast<const char*>(txt));
        }
        if (rc != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_read_db));
        }
        return paths;
    }

    // -----------------------------------------------------------------------
    // _appendFeedRowsFromDatabase
    // -----------------------------------------------------------------------

    void SqliteFeedStore::_appendFeedRowsFromDatabase(sqlite3* db,
                                                      const char* table_name,
                                                      const FeedQuery& query,
                                                      const std::optional<CursorKey>& before_key,
                                                      const std::size_t limit,
                                                      std::vector<FeedItem>& out_items,
                                                      std::unordered_set<std::string>& seen_feed_ids) const
    {
        // Rows from every ingestion source share one page, ordered by the global
        // (created_at_ms, block_number, tx_index, feed_id) key carried in the cursor.
        std::string sql = std::format(
            "SELECT "
            "feed_id, event_type, status, visible, tx_hash, block_number, tx_index, log_index, "
            "history_cursor, payload_json, created_at_ms, updated_at_ms, projector_version "
            "FROM {} WHERE visible=1",
            table_name);

        if (!query.include_unfinalized)
        {
            sql += " AND status='finalized'";
        }
        if (query.event_type.has_value())
        {
            sql += " AND event_type=?1";
        }

        int before_param_base = query.event_type.has_value() ? 2 : 1;
        if (before_key.has_value())
        {
            sql += std::format(" AND ("
                               "created_at_ms < ?{} "
                               "OR (created_at_ms = ?{} AND block_number < ?{}) "
                               "OR (created_at_ms = ?{} AND block_number = ?{} AND tx_index < ?{}) "
                               "OR (created_at_ms = ?{} AND block_number = ?{} AND tx_index = ?{} AND feed_id < ?{})"
                               ")",
                               before_param_base,
                               before_param_base + 1,
                               before_param_base + 2,
                               before_param_base + 3,
                               before_param_base + 4,
                               before_param_base + 5,
                               before_param_base + 6,
                               before_param_base + 7,
                               before_param_base + 8,
                               before_param_base + 9);
        }

        int limit_param = before_param_base;
        if (before_key.has_value())
        {
            limit_param += 10;
        }
        sql +=
            std::format(" ORDER BY created_at_ms DESC, block_number DESC, tx_index DESC, feed_id DESC LIMIT ?{};", limit_param);

        storage::sqlite::Statement stmt(db, sql.c_str());

        int param_index = 1;
        if (query.event_type.has_value())
        {
            sqlite3_bind_text(stmt.get(),
                              param_index,
                              query.event_type->c_str(),
                              static_cast<int>(query.event_type->size()),
                              SQLITE_TRANSIENT);
            ++param_index;
        }

        if (before_key.has_value())
        {
            const sqlite3_int64 before_created = static_cast<sqlite3_int64>(before_key->created_at_ms);
            const sqlite3_int64 before_block = static_cast<sqlite3_int64>(before_key->block_number);
            const sqlite3_int64 before_tx = static_cast<sqlite3_int64>(before_key->tx_index);

            sqlite3_bind_int64(stmt.get(), param_index++, before_created);
            sqlite3_bind_int64(stmt.get(), param_index++, before_created);
            sqlite3_bind_int64(stmt.get(), param_index++, before_block);
            sqlite3_bind_int64(stmt.get(), param_index++, before_created);
            sqlite3_bind_int64(stmt.get(), param_index++, before_block);
            sqlite3_bind_int64(stmt.get(), param_index++, before_tx);
            sqlite3_bind_int64(stmt.get(), param_index++, before_created);
            sqlite3_bind_int64(stmt.get(), param_index++, before_block);
            sqlite3_bind_int64(stmt.get(), param_index++, before_tx);
            sqlite3_bind_text(stmt.get(),
                              param_index++,
                              before_key->feed_id.c_str(),
                              static_cast<int>(before_key->feed_id.size()),
                              SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(stmt.get(), param_index, static_cast<sqlite3_int64>(limit));

        int rc = SQLITE_OK;
        while ((rc = stmt.step()) == SQLITE_ROW)
        {
            FeedItem item {};
            item.feed_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));

            if (seen_feed_ids.contains(item.feed_id))
            {
                continue;
            }

            seen_feed_ids.insert(item.feed_id);
            item.event_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
            item.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
            item.visible = sqlite3_column_int(stmt.get(), 3) != 0;
            item.tx_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
            item.block_number = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 5));
            item.tx_index = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 6));
            item.log_index = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 7));
            item.history_cursor = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
            const std::string payload_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 9));
            item.created_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 10));
            item.updated_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 11));
            item.projector_version = sqlite3_column_int(stmt.get(), 12);
            item.payload = json::parse(payload_json, nullptr, false);

            if (item.payload.is_discarded())
            {
                item.payload = json::object();
            }

            if (before_key.has_value() && !_cursorLessInDescOrder(item, *before_key))
            {
                continue;
            }

            out_items.push_back(std::move(item));
        }
        if (rc != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }

    // -----------------------------------------------------------------------
    // getFeedCursor / setFeedCursor
    // -----------------------------------------------------------------------

    std::int64_t SqliteFeedStore::getFeedCursor() const
    {
        // Reads from _write_db, not _read_db: _read_db is shared with concurrent
        // query-side callers (getFeedPage/getStreamPage) on other threads. A
        // still-open read transaction on that connection pins its snapshot, so a
        // cursor read interleaved on the same handle can observe a stale
        // pre-commit value and cause the projector to reprocess already-applied
        // rows. _write_db is only ever touched by the serialized write path, so
        // reading the cursor there always reflects the writer's own commits.
        storage::sqlite::Statement stmt(
            _write_db,
            "SELECT last_change_seq FROM feed_cursor WHERE singleton=1;");

        if (stmt.step() == SQLITE_ROW)
        {
            return static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 0));
        }
        return 0;
    }

    void SqliteFeedStore::setFeedCursor(std::int64_t last_change_seq)
    {
        storage::sqlite::Statement stmt(
            _write_db,
            "UPDATE feed_cursor SET last_change_seq=?1 WHERE singleton=1;");

        sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(last_change_seq));

        if (stmt.step() != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(_write_db));
        }
    }

    // -----------------------------------------------------------------------
    // maintainOutbox
    // -----------------------------------------------------------------------

    void SqliteFeedStore::maintainOutbox(std::int64_t now_ms)
    {
        if (!storage::sqlite::exec(_write_db, "BEGIN IMMEDIATE TRANSACTION;"))
        {
            spdlog::error("SqliteFeedStore::maintainOutbox: BEGIN IMMEDIATE failed");
            return;
        }

        try
        {
            if (_outbox_retention_ms > 0)
            {
                const std::int64_t cutoff_ms = now_ms - _outbox_retention_ms;
                std::int64_t max_pruned_seq = 0;

                storage::sqlite::Statement max_pruned_stmt(
                    _write_db,
                    "SELECT COALESCE(MAX(stream_seq), 0) FROM global_outbox WHERE created_at_ms < ?1;");

                sqlite3_bind_int64(max_pruned_stmt.get(), 1, static_cast<sqlite3_int64>(cutoff_ms));
                if (max_pruned_stmt.step() == SQLITE_ROW)
                {
                    max_pruned_seq = static_cast<std::int64_t>(sqlite3_column_int64(max_pruned_stmt.get(), 0));
                }

                storage::sqlite::Statement prune_outbox_stmt(
                    _write_db,
                    "DELETE FROM global_outbox WHERE created_at_ms < ?1;");

                sqlite3_bind_int64(prune_outbox_stmt.get(), 1, static_cast<sqlite3_int64>(cutoff_ms));
                if (prune_outbox_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }

                if (max_pruned_seq > 0)
                {
                    storage::sqlite::Statement update_floor_stmt(
                        _write_db,
                        "UPDATE outbox_stream_state "
                        "SET replay_floor_seq=MAX(replay_floor_seq, ?1) "
                        "WHERE singleton=1;");

                    sqlite3_bind_int64(
                        update_floor_stmt.get(),
                        1,
                        static_cast<sqlite3_int64>(max_pruned_seq + 1));

                    if (update_floor_stmt.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }
            }

            storage::sqlite::Statement align_floor_stmt(
                _write_db,
                "UPDATE outbox_stream_state "
                "SET replay_floor_seq=CASE "
                "WHEN EXISTS(SELECT 1 FROM global_outbox) THEN "
                "MAX(replay_floor_seq, (SELECT MIN(stream_seq) FROM global_outbox)) "
                "ELSE replay_floor_seq "
                "END "
                "WHERE singleton=1;");

            if (align_floor_stmt.step() != SQLITE_DONE)
            {
                throw std::runtime_error(sqlite3_errmsg(_write_db));
            }

            if (!storage::sqlite::exec(_write_db, "COMMIT;"))
            {
                throw std::runtime_error("maintainOutbox: commit failed");
            }
        }
        catch (const std::exception& e)
        {
            spdlog::error("SqliteFeedStore::maintainOutbox failed: {}", e.what());
            (void)storage::sqlite::exec(_write_db, "ROLLBACK;");
        }
    }

    // -----------------------------------------------------------------------
    // applyChange  (body of projectFeedRow, minus the normalized_events_hot mark)
    // -----------------------------------------------------------------------

    void SqliteFeedStore::applyChange(const events::ChangeRecord & row, std::int64_t now_ms)
    {
        if (!storage::sqlite::exec(_write_db, "BEGIN IMMEDIATE TRANSACTION;"))
        {
            throw std::runtime_error("applyChange: BEGIN IMMEDIATE failed");
        }

        try
        {
            const std::uint64_t chain_id = row.chain_id;
            const std::int64_t log_index = row.log_index;
            const std::string& tx_hash = row.tx_hash;
            const std::int64_t block_number = row.block_number;
            const std::int64_t tx_index = row.tx_index;
            const std::optional<std::int64_t>& block_time = row.block_time;
            const std::string& event_type = row.event_type;
            const std::string& name = row.name;
            const std::string& owner = row.owner;
            const std::string& state = row.state;

            const bool logical_feed_identity = _usesLogicalFeedIdentity(event_type);
            const std::string feed_id = _projectedFeedId(
                row.chain_namespace,
                chain_id,
                tx_hash,
                log_index,
                event_type,
                name);
            const bool visible = (state != std::string(events::REMOVED_STATE));

            bool existed = false;
            bool stream_emitted = false;

            std::int64_t created_at_ms = now_ms;
            std::string existing_status;
            int existing_visible = visible ? 1 : 0;
            std::string existing_history_cursor;
            std::string existing_payload_json;

            {
                storage::sqlite::Statement existing_feed_stmt(
                    _write_db,
                    "SELECT created_at_ms, status, visible, history_cursor, payload_json, stream_emitted "
                    "FROM feed_items_hot WHERE feed_id=?1;");

                sqlite3_bind_text(
                    existing_feed_stmt.get(),
                    1,
                    feed_id.c_str(),
                    static_cast<int>(feed_id.size()),
                    SQLITE_TRANSIENT);

                const int existing_feed_rc = existing_feed_stmt.step();
                if (existing_feed_rc == SQLITE_ROW)
                {
                    existed = true;
                    created_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(existing_feed_stmt.get(), 0));
                    existing_status = reinterpret_cast<const char*>(sqlite3_column_text(existing_feed_stmt.get(), 1));
                    existing_visible = sqlite3_column_int(existing_feed_stmt.get(), 2);
                    existing_history_cursor = reinterpret_cast<const char*>(sqlite3_column_text(existing_feed_stmt.get(), 3));
                    existing_payload_json = reinterpret_cast<const char*>(sqlite3_column_text(existing_feed_stmt.get(), 4));
                    stream_emitted = sqlite3_column_int(existing_feed_stmt.get(), 5) != 0;
                }
                else if (existing_feed_rc != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
            }

            const std::string history_cursor = existed
                ? existing_history_cursor
                : std::format("{}", CursorKey{
                      .chain_id = chain_id,
                      .chain_namespace = row.chain_namespace,
                      .created_at_ms = created_at_ms,
                      .block_number = block_number,
                      .tx_index = tx_index,
                      .feed_id = feed_id
                  });

            const std::string payload_json = _compactFeedPayloadJson(event_type, name, owner);

            const bool materially_changed =
                !existed ||
                existing_status != state ||
                existing_visible != (visible ? 1 : 0) ||
                (!logical_feed_identity && existing_history_cursor != history_cursor) ||
                existing_payload_json != payload_json;

            {
                storage::sqlite::Statement upsert_feed_stmt(
                    _write_db,
                    "INSERT INTO feed_items_hot("
                    "feed_id, chain_id, tx_hash, log_index, block_number, tx_index, block_time, event_type, status, visible, "
                    "history_cursor, payload_json, created_at_ms, updated_at_ms, projector_version, exported, source_id, chain_namespace"
                    ") VALUES("
                    "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, "
                    "?11, ?12, ?13, ?14, ?15, 0, ?16, ?17"
                    ") ON CONFLICT(feed_id) DO UPDATE SET "
                    "chain_id=excluded.chain_id, tx_hash=excluded.tx_hash, log_index=excluded.log_index, "
                    "source_id=excluded.source_id, chain_namespace=excluded.chain_namespace, "
                    "block_number=excluded.block_number, tx_index=excluded.tx_index, "
                    "block_time=excluded.block_time, event_type=excluded.event_type, status=excluded.status, "
                    "visible=excluded.visible, history_cursor=excluded.history_cursor, "
                    "payload_json=excluded.payload_json, updated_at_ms=excluded.updated_at_ms, "
                    "projector_version=excluded.projector_version, "
                    "stream_emitted=feed_items_hot.stream_emitted, "
                    "exported=CASE "
                    "WHEN feed_items_hot.status IS NOT excluded.status "
                    "OR feed_items_hot.visible IS NOT excluded.visible "
                    "OR feed_items_hot.history_cursor IS NOT excluded.history_cursor "
                    "OR feed_items_hot.payload_json IS NOT excluded.payload_json "
                    "THEN 0 ELSE feed_items_hot.exported END;");

                sqlite3_bind_text(
                    upsert_feed_stmt.get(), 1, feed_id.c_str(), static_cast<int>(feed_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(upsert_feed_stmt.get(), 2, static_cast<sqlite3_int64>(chain_id));
                sqlite3_bind_text(
                    upsert_feed_stmt.get(), 3, tx_hash.c_str(), static_cast<int>(tx_hash.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(upsert_feed_stmt.get(), 4, static_cast<sqlite3_int64>(log_index));
                sqlite3_bind_int64(upsert_feed_stmt.get(), 5, static_cast<sqlite3_int64>(block_number));
                sqlite3_bind_int64(upsert_feed_stmt.get(), 6, static_cast<sqlite3_int64>(tx_index));
                _bindOptionalInt64(upsert_feed_stmt.get(), 7, block_time);
                sqlite3_bind_text(
                    upsert_feed_stmt.get(), 8, event_type.c_str(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    upsert_feed_stmt.get(), 9, state.c_str(), static_cast<int>(state.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int(upsert_feed_stmt.get(), 10, visible ? 1 : 0);
                sqlite3_bind_text(
                    upsert_feed_stmt.get(), 11, history_cursor.c_str(), static_cast<int>(history_cursor.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    upsert_feed_stmt.get(), 12, payload_json.c_str(), static_cast<int>(payload_json.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(upsert_feed_stmt.get(), 13, static_cast<sqlite3_int64>(created_at_ms));
                sqlite3_bind_int64(upsert_feed_stmt.get(), 14, static_cast<sqlite3_int64>(now_ms));
                sqlite3_bind_int(upsert_feed_stmt.get(), 15, CURRENT_PROJECTOR_VERSION);
                sqlite3_bind_text(upsert_feed_stmt.get(), 16, row.source_id.c_str(), static_cast<int>(row.source_id.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(upsert_feed_stmt.get(), 17, row.chain_namespace.c_str(), static_cast<int>(row.chain_namespace.size()), SQLITE_TRANSIENT);

                if (upsert_feed_stmt.step() != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(_write_db));
                }
            }

            const bool should_emit_outbox =
                (visible && materially_changed) ||
                (!visible && stream_emitted && materially_changed);

            if (should_emit_outbox)
            {
                std::int64_t stream_seq = 0;

                {
                    storage::sqlite::Statement outbox_seq_select_stmt(
                        _write_db,
                        "SELECT next_stream_seq FROM outbox_stream_state WHERE singleton=1;");

                    const int outbox_seq_rc = outbox_seq_select_stmt.step();
                    if (outbox_seq_rc == SQLITE_ROW)
                    {
                        stream_seq = static_cast<std::int64_t>(sqlite3_column_int64(outbox_seq_select_stmt.get(), 0));
                    }
                    else if (outbox_seq_rc != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }

                if (stream_seq <= 0)
                {
                    throw std::runtime_error("invalid outbox stream sequencer state");
                }

                {
                    storage::sqlite::Statement outbox_seq_advance_stmt(
                        _write_db,
                        "UPDATE outbox_stream_state SET next_stream_seq=?1 WHERE singleton=1;");

                    sqlite3_bind_int64(
                        outbox_seq_advance_stmt.get(),
                        1,
                        static_cast<sqlite3_int64>(stream_seq + 1));

                    if (outbox_seq_advance_stmt.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }

                const std::string op =
                    (!visible) ? "remove" : (stream_emitted ? "update" : "insert");

                {
                    storage::sqlite::Statement insert_outbox_stmt(
                        _write_db,
                        "INSERT INTO global_outbox(stream_seq, feed_id, op, status, event_type, history_cursor, payload_json, "
                        "created_at_ms, source_id, chain_namespace) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);");

                    sqlite3_bind_int64(insert_outbox_stmt.get(), 1, static_cast<sqlite3_int64>(stream_seq));
                    sqlite3_bind_text(
                        insert_outbox_stmt.get(), 2, feed_id.c_str(), static_cast<int>(feed_id.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        insert_outbox_stmt.get(), 3, op.c_str(), static_cast<int>(op.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        insert_outbox_stmt.get(), 4, state.c_str(), static_cast<int>(state.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        insert_outbox_stmt.get(), 5, event_type.c_str(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        insert_outbox_stmt.get(), 6, history_cursor.c_str(), static_cast<int>(history_cursor.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        insert_outbox_stmt.get(), 7, payload_json.c_str(), static_cast<int>(payload_json.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_int64(insert_outbox_stmt.get(), 8, static_cast<sqlite3_int64>(now_ms));
                    sqlite3_bind_text(insert_outbox_stmt.get(), 9, row.source_id.c_str(), static_cast<int>(row.source_id.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_text(insert_outbox_stmt.get(), 10, row.chain_namespace.c_str(), static_cast<int>(row.chain_namespace.size()), SQLITE_TRANSIENT);

                    if (insert_outbox_stmt.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }

                {
                    storage::sqlite::Statement mark_stream_emitted_stmt(
                        _write_db,
                        "UPDATE feed_items_hot "
                        "SET stream_emitted=1 "
                        "WHERE feed_id=?1;");

                    sqlite3_bind_text(
                        mark_stream_emitted_stmt.get(),
                        1,
                        feed_id.c_str(),
                        static_cast<int>(feed_id.size()),
                        SQLITE_TRANSIENT);

                    if (mark_stream_emitted_stmt.step() != SQLITE_DONE)
                    {
                        throw std::runtime_error(sqlite3_errmsg(_write_db));
                    }
                }
            }

            if (!storage::sqlite::exec(_write_db, "COMMIT;"))
            {
                throw std::runtime_error("applyChange: COMMIT failed");
            }
        }
        catch (const std::exception& e)
        {
            spdlog::error("SqliteFeedStore::applyChange failed: {}", e.what());
            (void)storage::sqlite::exec(_write_db, "ROLLBACK;");
            throw;
        }
    }

} // namespace dcn::feed
