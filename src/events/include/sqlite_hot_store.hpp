#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "sqlite/wal.hpp"

#include "event_projector.hpp"
#include "events_store.hpp"

namespace dcn::events
{
    class SQLiteHotStore final : public IHotEventStore
    {
        public:
            SQLiteHotStore(
                const std::filesystem::path & hot_db_path,
                const std::uint64_t default_chain_id);

            ~SQLiteHotStore() override;

            // ---- hot DB / serialized by runtime strand / synchronous ----

            std::optional<std::uint64_t> loadNextCursor(
                const std::string & source_id,
                CursorKind kind) override;

            bool ingestBatch(
                    const std::string & source_id,
                    CursorKind kind,
                    const std::uint64_t chain_id,
                    const std::vector<RawChainLog> & raw_events,
                    const std::vector<DecodedEvent> & decoded_events,
                    const std::vector<ChainBlockInfo> & block_infos,
                    std::uint64_t next_cursor,
                    const std::int64_t now_ms,
                    const std::optional<ScannedBlockRange> & scanned_range = std::nullopt) override;

            bool ingestBatch(
                    const std::string & source_id,
                    const std::uint64_t chain_id,
                    const std::vector<DecodedEvent> & events,
                    const std::vector<ChainBlockInfo> & block_infos,
                    std::uint64_t next_cursor,
                    const std::int64_t now_ms);

            bool applyFinality(
                    const std::string & source_id,
                    const std::uint64_t chain_id,
                    const FinalityHeights & heights,
                    const std::int64_t now_ms,
                    const std::size_t reorg_window_blocks) override;

            // ---- min-cursor pruning / write-strand only ----

            std::size_t pruneConsumedRaw(
                    const std::string & source_id,
                    std::int64_t watermark,
                    std::int64_t finalized_floor_block,
                    std::size_t batch_limit);

            std::int64_t loadHeadBlock(const std::string & source_id);
            FinalityCheckpoint loadFinalityCheckpoint(const std::string & source_id);

            storage::sqlite::WalCheckpointStats checkpointWal(storage::sqlite::WalCheckpointMode mode);

            // ---- dead letters / write-strand only ----
            // projector_bit is one of the *_DEAD_LETTER_BIT constants. A marked row is
            // exempt from pruneConsumedRaw until every marking projector clears its bit.
            // markDeadLetter returns false when the row does not exist or the write failed,
            // in which case the caller must keep its cursor parked instead of skipping.

            bool markDeadLetter(int projector_bit, const std::string & source_id, const std::string & block_hash, std::int64_t log_index);
            bool clearDeadLetter(int projector_bit, const std::string & source_id, const std::string & block_hash, std::int64_t log_index);

            // ---- read side / synchronous ----

            std::vector<ChangeRecord> readChangesSince(std::int64_t after_change_seq, std::size_t limit) const;
            std::vector<ChangeRecord> readDeadLetters(int projector_bit, std::size_t limit) const;

        private:
            bool _initializeHotSchema();
            std::int64_t _nextChangeSeq();

            // Removes rows the scan proved gone. Must run inside the caller's transaction.
            void _reconcileScannedRange(
                    const std::string & source_id,
                    const ScannedBlockRange & range,
                    const std::vector<RawChainLog> & returned_logs,
                    std::int64_t now_ms);

        private:
            std::filesystem::path _hot_db_path;

            sqlite3 * _write_db = nullptr;
            sqlite3 * _read_db = nullptr;

            std::uint64_t _default_chain_id = 1;
    };
}
