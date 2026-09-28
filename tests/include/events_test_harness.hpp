#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "decentralised_art.hpp"
#include "feed.hpp"
#include "feed_projector.hpp"

#ifndef DECENTRALISED_ART_TEST_BINARY_DIR
    #error "DECENTRALISED_ART_TEST_BINARY_DIR is not defined"
#endif

namespace dcn::tests::events_harness
{
    using json = nlohmann::json;

    constexpr std::uint64_t CHAIN_ID = 1;

    template<class AwaitableT>
    auto runAwaitable(asio::io_context & io_context, AwaitableT awaitable)
    {
        auto future = asio::co_spawn(io_context, std::move(awaitable), asio::use_future);
        io_context.restart();
        io_context.run();
        return future.get();
    }

    inline std::filesystem::path buildPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_BINARY_DIR);
    }

    struct TempEventsPaths
    {
        std::filesystem::path root;
        std::filesystem::path hot_db;

        std::filesystem::path feed_db;
        std::filesystem::path feed_archive_root;

        ~TempEventsPaths()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    };

    inline TempEventsPaths makeTempEventsPaths(const std::string & test_name)
    {
        const std::string suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        TempEventsPaths paths{};
        paths.root = buildPath() / "tests" / "events" / (test_name + "_" + suffix);
        paths.hot_db = paths.root / "events_hot.sqlite";
        paths.feed_db = paths.root / "feed.sqlite";
        paths.feed_archive_root = paths.root / "feed_archive";

        std::error_code ec;
        std::filesystem::remove_all(paths.root, ec);
        ec.clear();
        std::filesystem::create_directories(paths.root, ec);
        if(ec)
        {
            throw std::runtime_error(std::format("Failed to create temp events test dir '{}': {}", paths.root.string(), ec.message()));
        }

        return paths;
    }

    class SqliteReadonly final
    {
        public:
            explicit SqliteReadonly(const std::filesystem::path & path)
            {
                const int open_rc = sqlite3_open_v2(
                    path.string().c_str(),
                    &_db,
                    SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                    nullptr);
                if(open_rc != SQLITE_OK)
                {
                    const std::string err = (_db == nullptr) ? "sqlite open failed" : sqlite3_errmsg(_db);
                    if(_db != nullptr)
                    {
                        sqlite3_close(_db);
                        _db = nullptr;
                    }
                    throw std::runtime_error(std::format("Failed to open sqlite db '{}': {}", path.string(), err));
                }

                sqlite3_busy_timeout(_db, 10'000);
            }

            ~SqliteReadonly()
            {
                if(_db != nullptr)
                {
                    sqlite3_close(_db);
                    _db = nullptr;
                }
            }

            SqliteReadonly(const SqliteReadonly &) = delete;
            SqliteReadonly & operator=(const SqliteReadonly &) = delete;

            std::int64_t scalarInt64(const std::string & sql) const
            {
                sqlite3_stmt * stmt = nullptr;
                if(sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(_db));
                }

                std::int64_t value = 0;
                if(sqlite3_step(stmt) == SQLITE_ROW)
                {
                    value = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 0));
                }
                sqlite3_finalize(stmt);
                return value;
            }

            std::string scalarText(const std::string & sql) const
            {
                sqlite3_stmt * stmt = nullptr;
                if(sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(_db));
                }

                std::string value;
                if(sqlite3_step(stmt) == SQLITE_ROW)
                {
                    const unsigned char * txt = sqlite3_column_text(stmt, 0);
                    if(txt != nullptr)
                    {
                        value = reinterpret_cast<const char *>(txt);
                    }
                }
                sqlite3_finalize(stmt);
                return value;
            }

            json scalarJson(const std::string & sql) const
            {
                const std::string txt = scalarText(sql);
                const json parsed = json::parse(txt, nullptr, false);
                if(parsed.is_discarded())
                {
                    return json::object();
                }
                return parsed;
            }

        private:
            sqlite3 * _db = nullptr;
    };

    class SqliteWritable final
    {
        public:
            explicit SqliteWritable(const std::filesystem::path & path)
            {
                const int open_rc = sqlite3_open_v2(
                    path.string().c_str(),
                    &_db,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                    nullptr);
                if(open_rc != SQLITE_OK)
                {
                    const std::string err = (_db == nullptr) ? "sqlite open failed" : sqlite3_errmsg(_db);
                    if(_db != nullptr)
                    {
                        sqlite3_close(_db);
                        _db = nullptr;
                    }
                    throw std::runtime_error(std::format("Failed to open sqlite db '{}': {}", path.string(), err));
                }
                sqlite3_busy_timeout(_db, 10'000);
            }

            ~SqliteWritable()
            {
                if(_db != nullptr)
                {
                    sqlite3_close(_db);
                    _db = nullptr;
                }
            }

            SqliteWritable(const SqliteWritable &) = delete;
            SqliteWritable & operator=(const SqliteWritable &) = delete;

            void exec(const std::string & sql) const
            {
                char * err = nullptr;
                const int rc = sqlite3_exec(_db, sql.c_str(), nullptr, nullptr, &err);
                if(rc != SQLITE_OK)
                {
                    const std::string err_msg = (err == nullptr) ? std::string(sqlite3_errmsg(_db)) : std::string(err);
                    if(err != nullptr)
                    {
                        sqlite3_free(err);
                    }
                    throw std::runtime_error(std::format("SQLite exec failed: {}", err_msg));
                }
            }

        private:
            sqlite3 * _db = nullptr;
    };

    inline std::int64_t rowCount(const std::filesystem::path & db_path, const std::string & table_name)
    {
        SqliteReadonly db(db_path);
        return db.scalarInt64(std::format("SELECT COUNT(1) FROM {};", table_name));
    }

    inline std::string hexBytes(const std::uint8_t value, const std::size_t bytes)
    {
        std::string out = "0x";
        out.reserve(2 + bytes * 2);
        constexpr char LUT[] = "0123456789abcdef";
        for(std::size_t i = 0; i < bytes; ++i)
        {
            out.push_back(LUT[(value >> 4) & 0x0F]);
            out.push_back(LUT[value & 0x0F]);
        }
        return out;
    }

    inline chain::Address makeAddressFromByte(const std::uint8_t value)
    {
        chain::Address address{};
        address.bytes[19] = value;
        return address;
    }

    inline std::string hexAddress(const std::uint8_t value)
    {
        return chain::normalizeHex(evmc::hex(makeAddressFromByte(value)));
    }

    inline std::string topicForEvent(const std::string & signature)
    {
        return chain::normalizeHex(evmc::hex(chain::constructEventTopic(signature)));
    }

    inline std::vector<std::uint8_t> encodeUint256Word(const std::uint64_t value)
    {
        std::vector<std::uint8_t> out(32, 0);
        for(int i = 0; i < 8; ++i)
        {
            out[31 - i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        }
        return out;
    }

    inline std::vector<std::uint8_t> encodeAddressWord(const chain::Address & value)
    {
        std::vector<std::uint8_t> out(32, 0);
        std::memcpy(out.data() + 12, value.bytes, 20);
        return out;
    }

    inline std::vector<std::uint8_t> encodeStringTail(const std::string & value)
    {
        std::vector<std::uint8_t> out = encodeUint256Word(value.size());
        out.insert(out.end(), value.begin(), value.end());
        const std::size_t pad = (32 - (value.size() % 32)) % 32;
        out.insert(out.end(), pad, 0);
        return out;
    }

    inline std::string toHexPrefixed(std::span<const std::uint8_t> bytes)
    {
        return std::string("0x") + evmc::hex(evmc::bytes_view{bytes.data(), bytes.size()});
    }

    // ABI payload for {Transformation,Condition}Added
    // (address caller, string name, address entityAddr, address owner, uint32 argsCount,
    //  bytes32 contentHash, bytes32 metadataHash, bytes32 runtimeCodeHash)
    inline std::string encodeSimpleAddedEventDataV2(
        const chain::Address & caller,
        const std::string & name,
        const chain::Address & entity_address,
        const chain::Address & owner,
        const std::uint32_t count,
        const std::uint8_t content_hash_fill = 0,
        const std::uint8_t metadata_hash_fill = 0,
        const std::uint8_t runtime_code_hash_fill = 0)
    {
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t> & word)
        {
            out.insert(out.end(), word.begin(), word.end());
        };

        append(encodeAddressWord(caller));
        append(encodeUint256Word(8 * 32));
        append(encodeAddressWord(entity_address));
        append(encodeAddressWord(owner));
        append(encodeUint256Word(count));
        append(std::vector<std::uint8_t>(32, content_hash_fill));
        append(std::vector<std::uint8_t>(32, metadata_hash_fill));
        append(std::vector<std::uint8_t>(32, runtime_code_hash_fill));
        append(encodeStringTail(name));

        return toHexPrefixed(std::span<const std::uint8_t>(out.data(), out.size()));
    }

    inline std::vector<std::uint8_t> encodeBytes32Word(const std::uint8_t fill)
    {
        return std::vector<std::uint8_t>(32, fill);
    }

    // ABI payload for {Transformation,Condition,Connector}Removed
    // (address caller, address owner, string name, address entityAddr, bytes32 contentHash, bytes32 metadataHash)
    inline std::string encodeRemovedEventData(
        const chain::Address & caller,
        const chain::Address & owner,
        const std::string & name,
        const chain::Address & entity_address,
        const std::uint8_t content_hash_fill,
        const std::uint8_t metadata_hash_fill)
    {
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t> & word)
        {
            out.insert(out.end(), word.begin(), word.end());
        };

        append(encodeAddressWord(caller));
        append(encodeAddressWord(owner));
        append(encodeUint256Word(6 * 32));
        append(encodeAddressWord(entity_address));
        append(encodeBytes32Word(content_hash_fill));
        append(encodeBytes32Word(metadata_hash_fill));
        append(encodeStringTail(name));

        return toHexPrefixed(std::span<const std::uint8_t>(out.data(), out.size()));
    }

    // ABI payload for EntityOwnerChanged
    // (uint8 kind, string name, address entityAddr, address oldOwner, address newOwner)
    inline std::string encodeOwnerChangedEventData(
        const std::uint8_t kind,
        const std::string & name,
        const chain::Address & entity_address,
        const chain::Address & old_owner,
        const chain::Address & new_owner)
    {
        std::vector<std::uint8_t> out;
        const auto append = [&out](const std::vector<std::uint8_t> & word)
        {
            out.insert(out.end(), word.begin(), word.end());
        };

        append(encodeUint256Word(kind));
        append(encodeUint256Word(5 * 32));
        append(encodeAddressWord(entity_address));
        append(encodeAddressWord(old_owner));
        append(encodeAddressWord(new_owner));
        append(encodeStringTail(name));

        return toHexPrefixed(std::span<const std::uint8_t>(out.data(), out.size()));
    }

    inline events::RawChainLog makeRawLog(
        const std::int64_t block_number,
        const std::int64_t tx_index,
        const std::int64_t log_index,
        std::string block_hash,
        std::string tx_hash,
        std::string topic0,
        std::string data_hex,
        const bool removed,
        const std::optional<std::int64_t> block_time,
        const std::int64_t seen_at_ms)
    {
        events::RawChainLog log{};
        log.chain_id = CHAIN_ID;
        log.block_number = block_number;
        log.block_hash = std::move(block_hash);
        log.parent_hash = hexBytes(0x7F, 32);
        log.tx_index = tx_index;
        log.tx_hash = std::move(tx_hash);
        log.log_index = log_index;
        log.address = hexAddress(0xAB);
        log.topics[0] = std::move(topic0);
        log.data_hex = std::move(data_hex);
        log.removed = removed;
        log.block_time = block_time;
        log.seen_at_ms = seen_at_ms;
        return log;
    }

    inline events::DecodedEvent makeDecodedEvent(
        const std::int64_t block_number,
        const std::int64_t tx_index,
        const std::int64_t log_index,
        const std::uint8_t block_hash_byte,
        const std::uint8_t tx_hash_byte,
        const events::EventType event_type,
        const events::EventState state = events::EventState::OBSERVED,
        const std::optional<std::int64_t> block_time = std::nullopt,
        const std::int64_t seen_at_ms = 1'000)
    {
        events::DecodedEvent event{};
        event.raw = makeRawLog(
            block_number,
            tx_index,
            log_index,
            hexBytes(block_hash_byte, 32),
            hexBytes(tx_hash_byte, 32),
            topicForEvent("TransformationAdded(address,string,address,address,uint32,bytes32,bytes32,bytes32)"),
            "0x00",
            state == events::EventState::REMOVED,
            block_time,
            seen_at_ms);
        event.event_type = event_type;
        event.state = state;
        event.name = std::format("entity_{}_{}", block_number, log_index);
        event.caller = hexAddress(0x41);
        event.owner = hexAddress(0x42);
        event.entity_address = hexAddress(0x43);
        event.args_count = 2;
        event.format_hash = hexBytes(0x11, 32);
        event.decoded_json = json{
            {"name", event.name},
            {"caller", event.caller},
            {"owner", event.owner},
            {"entity_address", event.entity_address}
        }.dump();
        return event;
    }

    inline events::ChainBlockInfo makeBlockInfo(
        const std::int64_t block_number,
        std::string block_hash,
        std::string parent_hash,
        const std::int64_t block_time,
        const std::int64_t seen_at_ms)
    {
        events::ChainBlockInfo info{};
        info.chain_id = CHAIN_ID;
        info.block_number = block_number;
        info.block_hash = std::move(block_hash);
        info.parent_hash = std::move(parent_hash);
        info.block_time = block_time;
        info.seen_at_ms = seen_at_ms;
        return info;
    }

    inline bool awaitIngestBatch(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        const std::uint64_t chain_id,
        const std::vector<events::DecodedEvent> & events,
        const std::vector<events::ChainBlockInfo> & block_infos,
        const std::int64_t next_from_block,
        const std::int64_t now_ms)
    {
        (void)io_context;
        return store.ingestBatch("local", chain_id, events, block_infos, next_from_block, now_ms);
    }

    inline bool awaitIngestBatch(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        const std::uint64_t chain_id,
        const std::vector<events::RawChainLog> & raw_events,
        const std::vector<events::DecodedEvent> & decoded_events,
        const std::vector<events::ChainBlockInfo> & block_infos,
        const std::int64_t next_from_block,
        const std::int64_t now_ms,
        const std::optional<std::uint64_t> next_local_seq = std::nullopt)
    {
        (void)io_context;
        return store.ingestBatch(
            "local",
            next_local_seq ? events::CursorKind::LOCAL_SEQUENCE : events::CursorKind::NEXT_BLOCK,
            chain_id,
            raw_events,
            decoded_events,
            block_infos,
            next_local_seq.value_or(static_cast<std::uint64_t>(next_from_block)),
            now_ms);
    }

    inline bool awaitApplyFinality(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        const std::uint64_t chain_id,
        const events::FinalityHeights & heights,
        const std::int64_t now_ms,
        const std::size_t reorg_window_blocks)
    {
        (void)io_context;
        return store.applyFinality("local", chain_id, heights, now_ms, reorg_window_blocks);
    }

    inline bool awaitApplyFinality(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        const std::uint64_t chain_id,
        const std::int64_t head,
        const std::int64_t safe,
        const std::int64_t finalized,
        const std::int64_t now_ms,
        const std::size_t reorg_window_blocks = 2048)
    {
        (void)io_context;
        return store.applyFinality(
            "local",
            chain_id,
            events::FinalityHeights{head, safe, finalized},
            now_ms,
            reorg_window_blocks);
    }

    inline bool awaitIngestBatchForSource(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        const std::string & source_id,
        const std::vector<events::DecodedEvent> & events,
        const std::vector<events::ChainBlockInfo> & block_infos,
        const std::uint64_t next_cursor,
        const std::int64_t now_ms,
        const std::uint64_t chain_id = CHAIN_ID)
    {
        (void)io_context;
        return store.ingestBatch(source_id, chain_id, events, block_infos, next_cursor, now_ms);
    }

    inline bool awaitApplyFinalityForSource(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        const std::string & source_id,
        const std::int64_t head,
        const std::int64_t safe,
        const std::int64_t finalized,
        const std::int64_t now_ms,
        const std::uint64_t chain_id = CHAIN_ID,
        const std::size_t reorg_window_blocks = 2048)
    {
        (void)io_context;
        return store.applyFinality(
            source_id,
            chain_id,
            events::FinalityHeights{head, safe, finalized},
            now_ms,
            reorg_window_blocks);
    }

    inline std::size_t awaitProjectBatch(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        feed::Feed & feed,
        const std::size_t limit,
        const std::int64_t now_ms)
    {
        auto strand = asio::make_strand(io_context);
        feed::FeedProjector projector(store, feed, strand);
        return runAwaitable(io_context, projector.projectBatch(limit, now_ms));
    }

    inline bool awaitRunArchiveCycle(
        asio::io_context & io_context,
        feed::Feed & feed,
        const std::uint64_t chain_id,
        const std::size_t hot_window_days,
        const std::int64_t now_ms)
    {
        return runAwaitable(io_context, feed.runArchiveCycle(chain_id, hot_window_days, now_ms));
    }

    inline std::optional<std::uint64_t> awaitLoadNextCursor(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        const events::CursorKind cursor_kind = events::CursorKind::NEXT_BLOCK,
        const std::string & source_id = "local")
    {
        (void)io_context;
        return store.loadNextCursor(source_id, cursor_kind);
    }

    inline storage::sqlite::WalCheckpointStats awaitCheckpointWalPassive(
        asio::io_context & io_context,
        events::SQLiteHotStore & store)
    {
        (void)io_context;
        return store.checkpointWal(storage::sqlite::WalCheckpointMode::PASSIVE);
    }

    inline storage::sqlite::WalCheckpointStats awaitCheckpointWalTruncate(
        asio::io_context & io_context,
        events::SQLiteHotStore & store)
    {
        (void)io_context;
        return store.checkpointWal(storage::sqlite::WalCheckpointMode::TRUNCATE);
    }

    inline std::size_t projectAll(
        asio::io_context & io_context,
        events::SQLiteHotStore & store,
        feed::Feed & feed,
        const std::int64_t now_ms)
    {
        std::size_t total = 0;
        for(std::size_t i = 0; i < 1024; ++i)
        {
            const std::size_t projected = awaitProjectBatch(io_context, store, feed, 512, now_ms);
            total += projected;
            if(projected == 0)
            {
                break;
            }
        }
        return total;
    }

    inline server::RouteArg makeUintQueryArg(const std::size_t value)
    {
        return server::RouteArg(
            server::RouteArgDef(server::RouteArgType::unsigned_integer, server::RouteArgRequirement::required),
            std::to_string(value));
    }

    inline server::RouteArg makeStringQueryArg(const std::string & value)
    {
        return server::RouteArg(
            server::RouteArgDef(server::RouteArgType::string, server::RouteArgRequirement::required),
            value);
    }

    struct SseFrame
    {
        bool is_comment = false;
        std::string comment;
        std::optional<std::int64_t> id = std::nullopt;
        std::string event;
        std::string data;
    };

    inline std::vector<SseFrame> parseSseFrames(const std::string & body)
    {
        std::vector<SseFrame> frames;

        std::size_t offset = 0;
        while(offset < body.size())
        {
            std::size_t block_end = body.find("\n\n", offset);
            if(block_end == std::string::npos)
            {
                block_end = body.size();
            }

            const std::string block = body.substr(offset, block_end - offset);
            offset = (block_end == body.size()) ? body.size() : block_end + 2;

            if(block.empty())
            {
                continue;
            }

            SseFrame frame{};
            std::istringstream lines(block);
            std::string line;
            while(std::getline(lines, line))
            {
                if(!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if(line.empty())
                {
                    continue;
                }

                if(line[0] == ':')
                {
                    frame.is_comment = true;
                    frame.comment = line.substr(1);
                    if(!frame.comment.empty() && frame.comment[0] == ' ')
                    {
                        frame.comment.erase(frame.comment.begin());
                    }
                    continue;
                }

                if(line.rfind("id:", 0) == 0)
                {
                    const std::string value = line.substr(3);
                    const auto trimmed = utils::trimAsciiWhitespace(value);
                    if(trimmed.has_value())
                    {
                        try
                        {
                            frame.id = std::stoll(*trimmed);
                        }
                        catch(...)
                        {
                            frame.id = std::nullopt;
                        }
                    }
                    continue;
                }

                if(line.rfind("event:", 0) == 0)
                {
                    frame.event = line.substr(6);
                    if(!frame.event.empty() && frame.event[0] == ' ')
                    {
                        frame.event.erase(frame.event.begin());
                    }
                    continue;
                }

                if(line.rfind("data:", 0) == 0)
                {
                    frame.data = line.substr(5);
                    if(!frame.data.empty() && frame.data[0] == ' ')
                    {
                        frame.data.erase(frame.data.begin());
                    }
                    continue;
                }
            }

            frames.push_back(std::move(frame));
        }

        return frames;
    }
}
