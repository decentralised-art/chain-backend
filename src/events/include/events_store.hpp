#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "decoded_event.hpp"
#include "events_ingest.hpp"

namespace dcn::events
{
    struct ChainBlockInfo
    {
        std::string source_id = "local";
        std::uint64_t chain_id = 1;
        std::int64_t block_number = 0;
        std::string block_hash;
        std::string parent_hash;
        std::int64_t block_time = 0;
        std::int64_t seen_at_ms = 0;
    };

    class IHotEventStore
    {
        public:
            virtual ~IHotEventStore() = default;

            virtual std::optional<std::uint64_t> loadNextCursor(
                const std::string & source_id,
                CursorKind kind) = 0;

            virtual bool ingestBatch(
                const std::string & source_id,
                CursorKind kind,
                std::uint64_t chain_id,
                const std::vector<RawChainLog> & raw_events,
                const std::vector<DecodedEvent> & decoded_events,
                const std::vector<ChainBlockInfo> & block_infos,
                std::uint64_t next_cursor,
                std::int64_t now_ms,
                const std::optional<ScannedBlockRange> & scanned_range = std::nullopt) = 0;

            virtual bool applyFinality(
                const std::string & source_id,
                std::uint64_t chain_id,
                const FinalityHeights & heights,
                std::int64_t now_ms,
                std::size_t reorg_window_blocks) = 0;
    };
}
