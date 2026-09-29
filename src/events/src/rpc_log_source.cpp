#include "rpc_log_source.hpp"

#include <algorithm>
#include <format>
#include <tuple>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "chain.hpp"
#include "pt_reads.hpp"
#include "registry_events.hpp"
#include "utils.hpp"

namespace dcn::events
{
    namespace
    {
        // How long a source that fell back to confirmation depth waits before asking for
        // the safe/finalized tags again; a fresh chain starts serving them once it finalizes.
        constexpr auto FINALITY_TAG_RETRY = std::chrono::minutes(10);

        // Only a deterministic answer to the tag request means the chain or endpoint does
        // not serve it. A transient fault, even one the provider reported as a JSON-RPC
        // error, says nothing about support.
        bool _tagUnsupported(const rpc::RpcError & error)
        {
            return error.kind == rpc::RpcError::Kind::EXECUTION
                || error.kind == rpc::RpcError::Kind::PROTOCOL;
        }
    }

    const std::vector<std::string> & ptRegistryTopics()
    {
        static const std::vector<std::string> topics{
            pt::eventTopic(pt::CONNECTOR_ADDED_SIGNATURE),
            pt::eventTopic(pt::TRANSFORMATION_ADDED_SIGNATURE),
            pt::eventTopic(pt::CONDITION_ADDED_SIGNATURE),
            pt::eventTopic(pt::CONNECTOR_REMOVED_SIGNATURE),
            pt::eventTopic(pt::TRANSFORMATION_REMOVED_SIGNATURE),
            pt::eventTopic(pt::CONDITION_REMOVED_SIGNATURE),
            pt::eventTopic(pt::ENTITY_OWNER_CHANGED_SIGNATURE),
            pt::eventTopic(pt::PROXY_UPGRADED_SIGNATURE)
        };
        return topics;
    }

    RpcLogSource::RpcLogSource(std::shared_ptr<rpc::EthClient> client, RpcLogSourceConfig config)
        : _client(std::move(client))
        , _config(std::move(config))
        , _upgraded_topic(pt::eventTopic(pt::PROXY_UPGRADED_SIGNATURE))
        , _batch_blocks(std::max<std::int64_t>(1, _config.max_batch_blocks))
    {
        _config.registry_address = chain::normalizeHex(_config.registry_address);
        if(!_config.expected_implementation_code_hash.empty())
        {
            _config.expected_implementation_code_hash = chain::normalizeHex(_config.expected_implementation_code_hash);
        }
    }

    std::string_view RpcLogSource::sourceId() const { return _config.source_id; }
    std::uint64_t RpcLogSource::chainId() const { return _config.chain_id; }
    std::string_view RpcLogSource::chainNamespace() const { return _config.chain_namespace; }
    CursorKind RpcLogSource::cursorKind() const { return CursorKind::NEXT_BLOCK; }
    bool RpcLogSource::paused() const { return _paused; }
    RpcLogSourceMetrics RpcLogSource::metrics() const { return _metrics; }

    void RpcLogSource::cancel()
    {
        _client->cancel();
    }

    void RpcLogSource::restoreFinality(const FinalityCheckpoint & checkpoint)
    {
        if(checkpoint.finalized_block < 0)
        {
            return;
        }

        _cycle = Cycle{
            .next = std::nullopt,
            .final_through = checkpoint.finalized_block,
            .applied_finalized = checkpoint.finalized_block,
            .anchor_hash = checkpoint.finalized_hash};
        _pending.reset();
        _metrics.applied_finalized = checkpoint.finalized_block;
    }

    void RpcLogSource::committed()
    {
        if(_pending)
        {
            _cycle = std::move(*_pending);
            _pending.reset();
            _metrics.applied_finalized = _cycle.applied_finalized;
        }
    }

    void RpcLogSource::_logMetrics()
    {
        const auto now = std::chrono::steady_clock::now();
        if(now < _metrics_logged_at + _config.metrics_log_interval)
        {
            return;
        }
        _metrics_logged_at = now;

        spdlog::info(
            "rpc source {}: polls={} failed={} ranges={} limit_hits={} blocks={} logs={} unparsable={} "
            "head={} lag={} finalized={}{}",
            _config.source_id,
            _metrics.polls,
            _metrics.failed_polls,
            _metrics.range_requests,
            _metrics.provider_limit_responses,
            _metrics.scanned_blocks,
            _metrics.logs_received,
            _metrics.unparsable_logs,
            _metrics.last_head,
            _metrics.lag_blocks,
            _metrics.applied_finalized,
            _paused ? " (paused)" : "");
    }

    asio::awaitable<std::optional<RpcLogSource::Heights>> RpcLogSource::_readHeights()
    {
        const auto latest = co_await _client->blockByTag(rpc::BLOCK_TAG_LATEST);
        if(!latest || !latest->has_value())
        {
            spdlog::warn(
                "rpc source {}: latest block unavailable: {}",
                _config.source_id,
                latest ? std::string("no block returned") : latest.error().message);
            co_return std::nullopt;
        }

        Heights heights{.head = **latest};
        const std::int64_t head = heights.head.number;

        if(!_finality_tags_supported && std::chrono::steady_clock::now() >= _finality_tags_retry_at)
        {
            _finality_tags_supported = true;
        }

        if(_finality_tags_supported)
        {
            const auto safe = co_await _client->blockByTag(rpc::BLOCK_TAG_SAFE);
            const auto finalized = co_await _client->blockByTag(rpc::BLOCK_TAG_FINALIZED);

            if((!safe && !_tagUnsupported(safe.error())) || (!finalized && !_tagUnsupported(finalized.error())))
            {
                spdlog::warn("rpc source {}: finality tags are temporarily unavailable", _config.source_id);
                co_return std::nullopt;
            }

            if(safe && finalized && safe->has_value() && finalized->has_value())
            {
                // A load-balanced provider can report finality from a node ahead of the one
                // that answered "latest"; nothing beyond the known head counts.
                const rpc::BlockHeader & finalized_header = **finalized;
                heights.finalized = std::min(finalized_header.number, head);
                heights.finalized_hash = finalized_header.number <= head ? finalized_header.hash : std::string{};
                heights.safe = std::clamp((*safe)->number, heights.finalized, head);
                co_return heights;
            }

            _finality_tags_supported = false;
            _finality_tags_retry_at = std::chrono::steady_clock::now() + FINALITY_TAG_RETRY;
            spdlog::warn(
                "rpc source {}: endpoint does not serve safe/finalized block tags, "
                "falling back to head - {} confirmations",
                _config.source_id,
                _config.confirmations);
        }

        heights.finalized = head - static_cast<std::int64_t>(_config.confirmations);
        heights.safe = heights.finalized;
        co_return heights;
    }

    asio::awaitable<std::optional<bool>> RpcLogSource::_anchorHolds(const std::int64_t head)
    {
        if(_cycle.anchor_hash.empty() || _cycle.applied_finalized < 0 || _cycle.applied_finalized > head)
        {
            co_return true;
        }

        const auto header = co_await _client->blockByNumber(_cycle.applied_finalized);
        if(!header || !header->has_value())
        {
            co_return std::nullopt;
        }

        co_return (*header)->hash == _cycle.anchor_hash;
    }

    asio::awaitable<std::optional<std::vector<rpc::json>>> RpcLogSource::_getLogsNarrowing(
        const std::int64_t from_block,
        const std::int64_t to_block,
        std::int64_t & scanned_to)
    {
        std::int64_t width = std::min<std::int64_t>(_batch_blocks, to_block - from_block + 1);

        while(width >= 1)
        {
            const std::int64_t attempt_to = from_block + width - 1;
            ++_metrics.range_requests;

            auto logs = co_await _client->getLogs(rpc::LogFilter{
                .from_block = from_block,
                .to_block = attempt_to,
                .address = _config.registry_address,
                .topic0 = ptRegistryTopics()
            });

            if(logs)
            {
                scanned_to = attempt_to;
                // Grow cautiously after a clean range so a provider that tightened its
                // limits is not immediately hammered with the old width again.
                if(width == _batch_blocks)
                {
                    _batch_blocks = std::min<std::int64_t>(
                        std::max<std::int64_t>(1, _config.max_batch_blocks), _batch_blocks + _batch_blocks / 2 + 1);
                }
                co_return std::move(*logs);
            }

            // Too many results and a provider timing out on a heavy range both call for
            // a narrower range rather than the same request again.
            const bool narrower_may_succeed = logs.error().kind == rpc::RpcError::Kind::LIMIT
                || logs.error().kind == rpc::RpcError::Kind::TIMEOUT;
            if(!narrower_may_succeed)
            {
                spdlog::warn(
                    "rpc source {}: eth_getLogs [{}, {}] failed: {}",
                    _config.source_id,
                    from_block,
                    attempt_to,
                    logs.error().message);
                co_return std::nullopt;
            }

            ++_metrics.provider_limit_responses;

            if(width == 1)
            {
                spdlog::error(
                    "rpc source {}: single block {} exceeds provider limits ({})",
                    _config.source_id,
                    from_block,
                    logs.error().message);
                co_return std::nullopt;
            }

            width /= 2;
            _batch_blocks = std::max<std::int64_t>(1, width);
            spdlog::info(
                "rpc source {}: provider limit hit, narrowing block range to {} blocks",
                _config.source_id,
                _batch_blocks);
        }

        co_return std::nullopt;
    }

    asio::awaitable<bool> RpcLogSource::_isSupportedImplementation()
    {
        const auto identity = co_await rpc::readPtIdentity(*_client, _config.registry_address);
        if(!identity)
        {
            spdlog::error(
                "rpc source {}: could not read protocol identity after proxy upgrade: {}",
                _config.source_id,
                identity.error().message);
            co_return false;
        }

        if(identity->protocol_id != rpc::ptProtocolId() ||
           identity->protocol_version != rpc::PT_PROTOCOL_VERSION)
        {
            spdlog::error(
                "rpc source {}: registry upgraded to unsupported protocol (id={}, version={})",
                _config.source_id,
                identity->protocol_id,
                identity->protocol_version);
            co_return false;
        }

        if(_config.expected_implementation_code_hash.empty())
        {
            co_return true;
        }

        const auto implementation = co_await rpc::readProxyImplementation(*_client, _config.registry_address);
        if(!implementation)
        {
            spdlog::error(
                "rpc source {}: could not read the registry implementation after proxy upgrade: {}",
                _config.source_id,
                implementation.error().message);
            co_return false;
        }

        const auto code_hash = co_await rpc::readCodeHash(*_client, *implementation);
        if(!code_hash || !code_hash->has_value() || **code_hash != _config.expected_implementation_code_hash)
        {
            spdlog::error(
                "rpc source {}: registry implementation {} is not the pinned implementation code",
                _config.source_id,
                *implementation);
            co_return false;
        }

        co_return true;
    }

    asio::awaitable<SourcePoll> RpcLogSource::pollSince(const std::uint64_t cursor, const std::size_t limit)
    {
        // The block range, not a record count, bounds a poll: `limit` would cut a
        // range in half mid-block and break the completeness the reconciliation relies on.
        (void)limit;

        // State from a poll the runtime did not commit is discarded, so that range is
        // scanned again.
        _pending.reset();

        SourcePoll poll;
        poll.next_cursor = cursor;
        ++_metrics.polls;
        _logMetrics();

        if(_paused)
        {
            co_return poll;
        }

        const auto heights = co_await _readHeights();
        if(!heights)
        {
            ++_metrics.failed_polls;
            co_return poll;
        }
        const std::int64_t head = heights->head.number;
        _metrics.last_head = head;

        const auto anchor = co_await _anchorHolds(head);
        if(!anchor)
        {
            ++_metrics.failed_polls;
            co_return poll;
        }
        if(!*anchor)
        {
            ++_metrics.failed_polls;
            _paused = true;
            spdlog::critical(
                "rpc source {}: block {} no longer has hash {}, which this server already finalized; "
                "ingestion is paused until the source is reindexed from a trusted provider",
                _config.source_id,
                _cycle.applied_finalized,
                _cycle.anchor_hash);
            co_return poll;
        }

        const std::int64_t next_block = std::max<std::int64_t>(static_cast<std::int64_t>(cursor), _config.deployment_block);

        Cycle cycle = _cycle;
        if(!cycle.next)
        {
            const std::int64_t cycle_start = std::max(_config.deployment_block, cycle.applied_finalized + 1);
            cycle.next = cycle_start;
            cycle.final_through = cycle_start - 1;
        }

        const std::int64_t from_block = *cycle.next;
        if(from_block > head)
        {
            // The cycle reached the head (or the provider is behind the cursor): the next
            // poll starts a new cycle at the finality boundary.
            cycle.next.reset();
            _pending = std::move(cycle);
            _metrics.lag_blocks = 0;
            poll.next_cursor = static_cast<std::uint64_t>(std::max(next_block, from_block));
            poll.finality = FinalityHeights{
                .head = head,
                .safe = std::max(_cycle.applied_finalized, std::min(heights->safe, from_block - 1)),
                .finalized = _cycle.applied_finalized,
                .finalized_hash = _cycle.anchor_hash};
            co_return poll;
        }

        const std::int64_t to_block = std::min(head, from_block + _batch_blocks - 1);
        std::int64_t scanned_to = from_block - 1;
        auto logs = co_await _getLogsNarrowing(from_block, to_block, scanned_to);
        if(!logs)
        {
            ++_metrics.failed_polls;
            co_return poll;
        }

        // A load-balanced provider can answer the log query from a node that has not seen
        // the whole range; the range only counts once the provider shows its last block.
        const auto end_header = co_await _client->blockByNumber(scanned_to);
        if(!end_header || !end_header->has_value())
        {
            ++_metrics.failed_polls;
            spdlog::info(
                "rpc source {}: provider does not serve block {} yet, rescanning",
                _config.source_id,
                scanned_to);
            co_return poll;
        }

        _metrics.scanned_blocks += static_cast<std::uint64_t>(scanned_to - from_block + 1);
        _metrics.logs_received += logs->size();

        const std::int64_t now_ms = utils::nowMs();

        std::vector<evm::EVM::EmittedLogRecord> records;
        records.reserve(logs->size());

        bool upgraded_seen = false;
        std::vector<std::int64_t> blocks_needing_header;

        for(const auto & log_json : *logs)
        {
            const auto raw = parse::parseRawLog(
                log_json, now_ms, _config.chain_id, _config.source_id, _config.chain_namespace);

            // Only logs the filter asked for may be ingested as registry events.
            if(!raw || raw->block_number < from_block || raw->block_number > scanned_to
               || raw->address != _config.registry_address || !raw->topics[0].has_value())
            {
                ++_metrics.unparsable_logs;
                spdlog::warn("rpc source {}: dropping a log the filter did not ask for", _config.source_id);
                continue;
            }

            // The proxy's own event is not a registry event: it only triggers the
            // implementation check below and is never stored.
            if(*raw->topics[0] == _upgraded_topic)
            {
                upgraded_seen = true;
                continue;
            }

            evm::EVM::EmittedLogRecord record{};
            record.block_number = raw->block_number;
            record.block_hash = raw->block_hash;
            record.tx_index = raw->tx_index;
            record.tx_hash = raw->tx_hash;
            record.log_index = raw->log_index;
            record.address = raw->address;
            record.data_hex = raw->data_hex;
            for(const auto & topic : raw->topics)
            {
                if(topic.has_value())
                {
                    record.topics.push_back(*topic);
                }
            }

            records.push_back(std::move(record));
            blocks_needing_header.push_back(raw->block_number);
        }

        std::ranges::sort(records, [](const auto & lhs, const auto & rhs)
        {
            return std::tie(lhs.block_number, lhs.tx_index, lhs.log_index)
                 < std::tie(rhs.block_number, rhs.tx_index, rhs.log_index);
        });

        // Headers are fetched only for blocks that actually matched, never for every
        // block in the range: an empty span must cost one eth_getLogs, not N requests.
        // ponytail: sequential fetch. Add bounded concurrency if backfill latency
        // becomes the bottleneck.
        std::ranges::sort(blocks_needing_header);
        const auto duplicates = std::ranges::unique(blocks_needing_header);
        blocks_needing_header.erase(duplicates.begin(), duplicates.end());

        std::unordered_map<std::int64_t, rpc::BlockHeader> headers;
        headers.emplace(scanned_to, **end_header);
        for(const std::int64_t block_number : blocks_needing_header)
        {
            if(headers.contains(block_number))
            {
                continue;
            }

            const auto header = co_await _client->blockByNumber(block_number);
            if(!header || !header->has_value())
            {
                ++_metrics.failed_polls;
                spdlog::warn(
                    "rpc source {}: could not read header for block {}, retrying range",
                    _config.source_id,
                    block_number);
                co_return poll;
            }
            headers.emplace(block_number, **header);
        }

        for(auto & record : records)
        {
            const rpc::BlockHeader & header = headers.at(record.block_number);
            if(header.hash != record.block_hash)
            {
                // The block was replaced between the log query and the header read;
                // leave the cursor where it is and rescan on the next poll.
                ++_metrics.failed_polls;
                spdlog::info(
                    "rpc source {}: block {} changed mid-poll, rescanning",
                    _config.source_id,
                    record.block_number);
                co_return poll;
            }

            record.parent_hash = header.parent_hash;
            record.block_time = header.timestamp;
        }

        if(upgraded_seen)
        {
            const bool supported = co_await _isSupportedImplementation();
            if(!supported)
            {
                ++_metrics.failed_polls;
                _paused = true;
                spdlog::error(
                    "rpc source {}: ingestion paused after an unrecognised registry upgrade; "
                    "restart with a server build and manifest that support the deployed implementation",
                    _config.source_id);
                co_return poll;
            }
        }

        // Only blocks scanned while the provider already reported them final, contiguous
        // from the cycle start, become final: a block scanned before it finalized may
        // still hold rows of a replaced fork until a later cycle rescans it.
        const std::int64_t final_in_range = std::min(scanned_to, heights->finalized);
        if(cycle.final_through == from_block - 1 && final_in_range >= from_block)
        {
            cycle.final_through = final_in_range;
        }
        // The cycle counts blocks before the deployment as final from its start; the
        // chain's own finalized height still bounds what is applied and anchored.
        const std::int64_t provably_final = std::min(cycle.final_through, heights->finalized);
        if(provably_final > cycle.applied_finalized)
        {
            cycle.applied_finalized = provably_final;
            cycle.anchor_hash.clear();
            if(cycle.applied_finalized == scanned_to)
            {
                cycle.anchor_hash = (*end_header)->hash;
            }
            else if(cycle.applied_finalized == heights->finalized && !heights->finalized_hash.empty())
            {
                cycle.anchor_hash = heights->finalized_hash;
            }
            else
            {
                const auto anchor_header = co_await _client->blockByNumber(cycle.applied_finalized);
                if(anchor_header && anchor_header->has_value())
                {
                    cycle.anchor_hash = (*anchor_header)->hash;
                }
            }
        }

        cycle.next = scanned_to + 1;
        if(scanned_to >= head)
        {
            cycle.next.reset();
        }

        poll.records = std::move(records);
        poll.next_cursor = static_cast<std::uint64_t>(std::max(next_block, scanned_to + 1));
        poll.scanned_range = ScannedBlockRange{.from_block = from_block, .to_block = scanned_to};
        poll.caught_up = scanned_to >= head;
        poll.finality = FinalityHeights{
            .head = head,
            .safe = std::max(cycle.applied_finalized, std::min(heights->safe, scanned_to)),
            .finalized = cycle.applied_finalized,
            .finalized_hash = cycle.anchor_hash};

        _metrics.lag_blocks = std::max<std::int64_t>(0, head - scanned_to);
        _pending = std::move(cycle);

        co_return poll;
    }
}
