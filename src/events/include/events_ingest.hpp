#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "decoded_event.hpp"

namespace dcn::events
{
    // How a source's persisted cursor is interpreted.
    enum class CursorKind : std::uint8_t
    {
        LOCAL_SEQUENCE,  // index into the in-process EVM's emitted-log sequence
        NEXT_BLOCK       // first block number not yet scanned
    };

    std::string_view toString(CursorKind kind);
    std::optional<CursorKind> parseCursorKind(std::string_view value);

    // The finality a source had applied when the process last stopped.
    struct FinalityCheckpoint
    {
        std::int64_t finalized_block = -1;
        std::string finalized_hash;
    };

    struct FinalityHeights
    {
        std::int64_t head = 0;
        std::int64_t safe = 0;
        std::int64_t finalized = 0;

        // Hash of block `finalized` when the source knows it: persisted as the anchor a
        // later poll uses to detect a reorg below finality.
        std::string finalized_hash{};
    };

    // Inclusive block span a poll scanned completely. Every log the source can decode
    // within it is present in that poll, so anything previously stored in the span and
    // absent now was reorged out.
    struct ScannedBlockRange
    {
        std::int64_t from_block = 0;
        std::int64_t to_block = -1;
    };

    class IEventDecoder
    {
        public:
            virtual ~IEventDecoder() = default;
            virtual std::optional<DecodedEvent> decode(const RawChainLog & log) const = 0;
    };

    // Decodes the registry events of submodule/pt. Rejects any event naming an entity
    // this server cannot represent (pt::isValidEntityName), so such an entity is kept
    // as raw evidence but never projected.
    class PTEventDecoder final : public IEventDecoder
    {
        public:
            PTEventDecoder();

            std::optional<DecodedEvent> decode(const RawChainLog & log) const override;

        private:
            std::string _connector_topic;
            std::string _transformation_topic;
            std::string _condition_topic;
            std::string _connector_removed_topic;
            std::string _transformation_removed_topic;
            std::string _condition_removed_topic;
            std::string _owner_changed_topic;
    };
}
