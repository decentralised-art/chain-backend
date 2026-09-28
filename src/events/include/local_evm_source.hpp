#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "emitted_log_source.hpp"
#include "evm.hpp"

namespace dcn::events
{
    // Adapts the in-process EVM into an EmittedLogRecord source. Keeps the evm
    // module generic: the ephemeral-entity rewrite stays a policy flag applied
    // by ingestion, not baked into the EVM. All blocks are reported finalized
    // immediately (local EVM has no reorg/finality staging).
    class LocalEvmSource final : public IEmittedLogSource
    {
        public:
            LocalEvmSource(evm::EVM & evm, std::uint64_t chain_id)
                : _evm(evm)
                , _chain_id(chain_id)
            {
            }

            std::string_view sourceId() const override { return LOCAL_SOURCE_ID; }
            std::uint64_t chainId() const override { return _chain_id; }
            std::string_view chainNamespace() const override { return LOCAL_CHAIN_NAMESPACE; }
            CursorKind cursorKind() const override { return CursorKind::LOCAL_SEQUENCE; }
            bool ephemeralEntities() const override { return true; }

            // Polled in-process with zero network latency, so a fast interval is safe and
            // necessary: the deploy POST handlers await registry materialization with a
            // 5 s budget, which a slow poll would consume before ingestion even starts.
            std::optional<std::chrono::milliseconds> pollInterval() const override
            {
                return std::chrono::milliseconds(100);
            }

            asio::awaitable<SourcePoll> pollSince(std::uint64_t cursor, std::size_t limit) override
            {
                std::vector<evm::EVM::EmittedLogRecord> records = co_await _evm.getLogsSince(cursor, limit);
                const std::size_t records_count = records.size();
                const std::int64_t head = co_await _evm.getHeadBlockNumber();

                std::int64_t max_block = std::max<std::int64_t>(head, 0);
                for(const auto & record : records)
                {
                    max_block = std::max<std::int64_t>(max_block, record.block_number);
                }

                // The cursor is the last sequence number consumed: the EVM returns the logs
                // after it, so the next log emitted is the next one read.
                const std::uint64_t next_cursor = records.empty() ? cursor : records.back().seq;

                co_return SourcePoll{
                    .records = std::move(records),
                    .finality = FinalityHeights{
                        .head = max_block,
                        .safe = max_block,
                        .finalized = max_block
                    },
                    .next_cursor = next_cursor,
                    .scanned_range = std::nullopt,
                    .caught_up = records_count < limit
                };
            }

        private:
            evm::EVM & _evm;
            std::uint64_t _chain_id;
    };
}
