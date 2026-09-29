#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio.hpp>

#include "evm.hpp"          // evm::EVM::EmittedLogRecord
#include "events_ingest.hpp" // FinalityHeights, ScannedBlockRange

namespace dcn::events
{
    // Result of a single poll against an attached log source.
    struct SourcePoll
    {
        std::vector<evm::EVM::EmittedLogRecord> records;

        // Finality the runtime may apply after committing this poll. Absent when the poll
        // proved nothing new, so a failed poll can never promote unreconciled rows.
        std::optional<FinalityHeights> finality = std::nullopt;

        std::uint64_t next_cursor = 0;

        // Set by sources that scan complete block ranges. Drives reorg reconciliation:
        // the runtime removes rows in the range that this poll did not return.
        std::optional<ScannedBlockRange> scanned_range = std::nullopt;

        // False while the source still has backlog to deliver, so ingestion keeps
        // polling instead of waiting out the idle interval between batches.
        bool caught_up = true;
    };

    // An attachable source of EmittedLogRecord batches for event ingestion. Each source
    // has a stable source_id that keys its cursor, finality and stored rows. Ingestion
    // drives the source (pull): it repeatedly calls pollSince() with the persisted cursor.
    class IEmittedLogSource
    {
        public:
            virtual ~IEmittedLogSource() = default;

            virtual std::string_view sourceId() const = 0;
            virtual std::uint64_t chainId() const = 0;

            // Namespace prefix for feed ids / cursors produced from this source's events.
            virtual std::string_view chainNamespace() const = 0;

            virtual CursorKind cursorKind() const = 0;

            // When true, ingestion rewrites decoded entity addresses to the
            // ephemeral sentinel for this source's events (local EVM policy).
            virtual bool ephemeralEntities() const { return false; }

            // Idle interval between polls once caught up; nullopt uses the runtime default.
            virtual std::optional<std::chrono::milliseconds> pollInterval() const { return std::nullopt; }

            // Called once before the first poll with the finality persisted for this source.
            virtual void restoreFinality(const FinalityCheckpoint & checkpoint) { (void)checkpoint; }

            virtual asio::awaitable<SourcePoll> pollSince(std::uint64_t cursor, std::size_t limit) = 0;

            // Called after the runtime durably committed the last poll's batch and cursor.
            // A source that advances internal state per poll applies it only here, so a
            // failed commit re-delivers the same range instead of skipping it.
            virtual void committed() {}

            // Aborts in-flight work so a stop request is not held up by a slow upstream.
            virtual void cancel() {}

            virtual asio::awaitable<void> stop() { co_return; }
    };
}
