#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "registry_projector.hpp"

#include "connector.hpp"
#include "transformation.hpp"
#include "condition.hpp"
#include "address.hpp"
#include "format_hash.hpp"
#include "hex.hpp"
#include "async.hpp"

namespace dcn::registry
{
    RegistryProjector::RegistryProjector(
        events::SQLiteHotStore & store,
        Registry & registry,
        const asio::strand<asio::io_context::executor_type> & write_strand,
        events::ProjectorRetryConfig retry)
        : _store(store)
        , _registry(registry)
        , _write_strand(write_strand)
        , _retry(retry)
    {
        // Load the persisted cursor so we resume from the right position after restart.
        _cursor = _registry.getMaterializationCursor();
    }

    std::string_view RegistryProjector::id() const
    {
        return events::REGISTRY_PROJECTOR_ID;
    }

    asio::awaitable<std::size_t> RegistryProjector::projectBatch(std::size_t limit, std::int64_t now_ms)
    {
        // Serialize the hot-store read on the write strand, as FeedProjector does.
        co_await async::ensureOnStrand(_write_strand);
        const auto rows = _store.readChangesSince(_cursor, limit);
        std::size_t done = 0;
        std::int64_t last_advanced_seq = _cursor;

        for(const auto & row : rows)
        {
            if(row.state != events::FINALIZED_STATE)
            {
                // Only finalized rows are materialized. observed/safe rows re-surface with
                // a strictly higher change_seq once finalized, so advancing past them here
                // is safe. A "removed" row is a reorg rollback — and a reorg can only touch
                // blocks inside the reorg window, i.e. rows that were never finalized and
                // therefore never materialized. So a removal can never strand a stale
                // registry row, and skipping it is correct.
                last_advanced_seq = row.change_seq;
                ++done;
                continue;
            }

            const bool ok = co_await _materializeOne(row);
            if(!ok)
            {
                if(row.change_seq == _failing_seq)
                {
                    ++_failure_count;
                }
                else
                {
                    _failing_seq = row.change_seq;
                    _failure_count = 1;
                }

                if(_failure_count < _retry.max_attempts)
                {
                    // Leave the cursor before this row so it is retried next pass;
                    // transient failures (e.g. busy registry DB) heal across passes.
                    break;
                }

                // Retry budget exhausted: dead-letter the row and advance past it rather
                // than wedge the pipeline (a permanently parked cursor freezes the prune
                // watermark for every projector, growing the hot store without bound).
                // The dead-letter bit exempts the row from pruning, so the raw chain
                // evidence is retained and the idle-time sweep keeps retrying it — no
                // chain event data is lost.
                co_await async::ensureOnStrand(_write_strand);
                if(!_store.markDeadLetter(
                    events::REGISTRY_DEAD_LETTER_BIT, row.source_id, row.block_hash, row.log_index))
                {
                    // The quarantine could not be made durable; advancing now would let
                    // pruning delete the row, so keep the cursor parked and retry.
                    spdlog::error("[RegistryProjector] failed to dead-letter change_seq={} — "
                        "cursor stays parked", row.change_seq);
                    break;
                }

                spdlog::error("[RegistryProjector] dead-lettered change_seq={} "
                    "(block={} log_index={} event_type='{}') after {} failed attempts; "
                    "row is retained in the hot store and retried by the sweep",
                    row.change_seq, row.block_number, row.log_index, row.event_type,
                    _failure_count);
                _failing_seq = 0;
                _failure_count = 0;
            }

            last_advanced_seq = row.change_seq;
            ++done;
        }

        if(last_advanced_seq > _cursor)
        {
            // The cursor write hops onto the Registry's own strand (inside
            // setMaterializationCursor), not the hot-store write strand, so it serializes
            // with every other registry DB access.
            //
            // Only advance the in-memory cursor once the write is durable. The prune loop
            // uses this cursor as the deletion watermark for raw source rows; advancing it
            // past an unpersisted write would let those rows be pruned before the registry
            // cursor catches up, dropping events on the floor after a restart.
            if(co_await _registry.setMaterializationCursor(last_advanced_seq))
            {
                _cursor = last_advanced_seq;
            }
            else
            {
                spdlog::error("[RegistryProjector] Failed to persist cursor seq={} — retrying next pass",
                    last_advanced_seq);
            }
        }

        // Retry dead-lettered rows only when the main changelog is drained, so the
        // sweep never competes with live projection.
        if(rows.empty())
        {
            done += co_await _sweepDeadLetters(limit, now_ms);
        }

        co_return done;
    }

    asio::awaitable<std::size_t> RegistryProjector::_sweepDeadLetters(std::size_t limit, std::int64_t now_ms)
    {
        if(now_ms - _last_sweep_ms < _retry.sweep_interval_ms)
        {
            co_return 0;
        }
        _last_sweep_ms = now_ms;

        co_await async::ensureOnStrand(_write_strand);
        const auto rows = _store.readDeadLetters(events::REGISTRY_DEAD_LETTER_BIT, limit);

        std::size_t resolved = 0;
        for(const auto & row : rows)
        {
            // A row reorg-removed after being dead-lettered has nothing left to
            // materialize; releasing it lets pruning take the row.
            const bool ok = (row.state != events::FINALIZED_STATE) || co_await _materializeOne(row);
            if(!ok)
            {
                // Still failing (e.g. unresolved divergence); the bit stays set, the
                // row stays retained, and the next sweep retries it.
                continue;
            }

            co_await async::ensureOnStrand(_write_strand);
            if(_store.clearDeadLetter(
                events::REGISTRY_DEAD_LETTER_BIT, row.source_id, row.block_hash, row.log_index))
            {
                spdlog::info("[RegistryProjector] dead-lettered change_seq={} resolved "
                    "(block={} log_index={} event_type='{}')",
                    row.change_seq, row.block_number, row.log_index, row.event_type);
                ++resolved;
            }
        }

        co_return resolved;
    }

    asio::awaitable<bool> RegistryProjector::_materializeOne(const events::ChangeRecord & row)
    {
        // Collect non-null topics in order; stop at first nullopt.
        std::vector<std::string> topics;
        for(const auto & t : row.topics)
        {
            if(!t.has_value())
            {
                break;
            }
            topics.push_back(*t);
        }

        // Chain versions are canonical: a remote event is written even when a local
        // version of the same name is already resolved, and the difference is recorded.
        // Local events keep the strict equality check that guards the
        // POST-then-project path.
        const bool local_source = row.source_id == events::LOCAL_SOURCE_ID;

        EntitySource source{
            .source_id = row.source_id,
            .chain_namespace = row.chain_namespace,
            .chain_id = row.chain_id,
            .registry_address = row.registry_address,
            .content_hash = row.content_hash.value_or(""),
            .metadata_hash = row.metadata_hash.value_or(""),
            .runtime_code_hash = row.runtime_code_hash.value_or(""),
            .block_hash = row.block_hash,
            .log_index = row.log_index
        };
        const auto materializedAddress = [&row](const chain::Address & decoded)
        {
            if(row.entity_address == events::EPHEMERAL_ENTITY_ADDRESS) return chain::Address{};
            return evmc::from_hex<chain::Address>(row.entity_address).value_or(decoded);
        };

        // A chain version whose artifact differs from the version currently resolved for
        // the name. Recorded for operators; the chain version still takes precedence.
        const auto definitionConflict = [&](const EntityKind kind, const std::string & name) -> asio::awaitable<bool>
        {
            if(local_source) co_return false;
            const auto existing = co_await _registry.getEntityLocation(kind, name);
            co_return existing
                && ((!existing->source.content_hash.empty() && existing->source.content_hash != source.content_hash)
                    || (!existing->source.metadata_hash.empty() && existing->source.metadata_hash != source.metadata_hash));
        };

        const auto kindFromEvent = [](const std::string_view event_type) -> std::optional<EntityKind>
        {
            if(event_type == events::CONNECTOR_REMOVED_TYPE) return EntityKind::CONNECTOR;
            if(event_type == events::TRANSFORMATION_REMOVED_TYPE) return EntityKind::TRANSFORMATION;
            if(event_type == events::CONDITION_REMOVED_TYPE) return EntityKind::CONDITION;
            return std::nullopt;
        };

        if(const auto kind = kindFromEvent(row.event_type))
        {
            co_return co_await _registry.removeEntityVersion(*kind, row.source_id, row.name);
        }

        if(row.event_type == events::ENTITY_OWNER_CHANGED_TYPE)
        {
            if(!row.entity_kind || !row.old_owner) co_return false;
            const EntityKind kind = *row.entity_kind;
            const auto old_owner = evmc::from_hex<chain::Address>(*row.old_owner);
            const auto new_owner = evmc::from_hex<chain::Address>(row.owner);
            if(!old_owner || !new_owner) co_return false;
            co_return co_await _registry.changeEntityOwner(
                kind, row.source_id, row.name, *old_owner, *new_owner);
        }

        // ---- transformation_added ----
        if(row.event_type == events::TRANSFORMATION_ADDED_TYPE)
        {
            const auto event = pt::decodeTransformationAddedEvent(row.data_hex, topics);
            if(!event)
            {
                spdlog::error("[RegistryProjector] Undecodable transformation_added at block={} log_index={} — "
                    "will retry, then poison-skip",
                    row.block_number, row.log_index);
                co_return false;
            }

            if(const auto existing = co_await _registry.getTransformationRecordHandle(event->name))
            {
                source.definition_conflict = co_await definitionConflict(EntityKind::TRANSFORMATION, event->name);
                const TransformationRecord & rec = **existing;
                const bool diverges = rec.transformation().args_count() != event->args_count
                    || chain::normalizeHex(rec.owner()) != chain::addressToHex(event->owner);
                if(local_source)
                {
                    if(diverges)
                    {
                        spdlog::error("[RegistryProjector] transformation '{}' diverges from chain "
                            "(existing owner={} args={}, chain owner={} args={}) — will retry, then poison-skip",
                            event->name, rec.owner(), rec.transformation().args_count(),
                            chain::addressToHex(event->owner), event->args_count);
                        co_return false;
                    }
                    co_return true; // already present and matches — idempotent
                }
                if(diverges)
                {
                    spdlog::warn("[RegistryProjector] transformation '{}' on source '{}' differs from the "
                        "resolved version (resolved owner={} args={}, chain owner={} args={}); "
                        "the chain version takes precedence",
                        event->name, row.source_id, rec.owner(), rec.transformation().args_count(),
                        chain::addressToHex(event->owner), event->args_count);
                }
            }

            TransformationRecord record;
            record.mutable_transformation()->set_name(event->name);
            record.mutable_transformation()->set_args_count(event->args_count);
            record.set_owner(chain::addressToHex(event->owner));

            co_return co_await _registry.addTransformationVersion(
                source, materializedAddress(event->transformation_address), std::move(record));
        }

        // ---- condition_added ----
        if(row.event_type == events::CONDITION_ADDED_TYPE)
        {
            const auto event = pt::decodeConditionAddedEvent(row.data_hex, topics);
            if(!event)
            {
                spdlog::error("[RegistryProjector] Undecodable condition_added at block={} log_index={} — "
                    "will retry, then poison-skip",
                    row.block_number, row.log_index);
                co_return false;
            }

            if(const auto existing = co_await _registry.getConditionRecordHandle(event->name))
            {
                source.definition_conflict = co_await definitionConflict(EntityKind::CONDITION, event->name);
                const ConditionRecord & rec = **existing;
                const bool diverges = rec.condition().args_count() != event->args_count
                    || chain::normalizeHex(rec.owner()) != chain::addressToHex(event->owner);
                if(local_source)
                {
                    if(diverges)
                    {
                        spdlog::error("[RegistryProjector] condition '{}' diverges from chain "
                            "(existing owner={} args={}, chain owner={} args={}) — will retry, then poison-skip",
                            event->name, rec.owner(), rec.condition().args_count(),
                            chain::addressToHex(event->owner), event->args_count);
                        co_return false;
                    }
                    co_return true;
                }
                if(diverges)
                {
                    spdlog::warn("[RegistryProjector] condition '{}' on source '{}' differs from the "
                        "resolved version (resolved owner={} args={}, chain owner={} args={}); "
                        "the chain version takes precedence",
                        event->name, row.source_id, rec.owner(), rec.condition().args_count(),
                        chain::addressToHex(event->owner), event->args_count);
                }
            }

            ConditionRecord record;
            record.mutable_condition()->set_name(event->name);
            record.mutable_condition()->set_args_count(event->args_count);
            record.set_owner(chain::addressToHex(event->owner));

            co_return co_await _registry.addConditionVersion(
                source, materializedAddress(event->condition_address), std::move(record));
        }

        // ---- connector_added ----
        if(row.event_type == events::CONNECTOR_ADDED_TYPE)
        {
            const auto event = pt::decodeConnectorAddedEvent(row.data_hex, topics);
            if(!event)
            {
                spdlog::error("[RegistryProjector] Undecodable connector_added at block={} log_index={} — "
                    "will retry, then poison-skip",
                    row.block_number, row.log_index);
                co_return false;
            }

            if(const auto existing = co_await _registry.getConnectorRecordHandle(event->name))
            {
                source.definition_conflict = co_await definitionConflict(EntityKind::CONNECTOR, event->name);
                const ConnectorRecord & rec = **existing;
                const bool diverges = chain::normalizeHex(rec.owner()) != chain::addressToHex(event->owner);
                if(local_source)
                {
                    if(diverges)
                    {
                        spdlog::error("[RegistryProjector] connector '{}' diverges from chain "
                            "(existing owner={}, chain owner={}) — will retry, then poison-skip",
                            event->name, rec.owner(), chain::addressToHex(event->owner));
                        co_return false;
                    }
                    co_return true;
                }
                if(diverges)
                {
                    spdlog::warn("[RegistryProjector] connector '{}' on source '{}' differs from the "
                        "resolved version (resolved owner={}, chain owner={}); "
                        "the chain version takes precedence",
                        event->name, row.source_id, rec.owner(), chain::addressToHex(event->owner));
                }
            }

            // Pass the chain-emitted format hash so the registry refuses to store a locally-recomputed
            // hash that disagrees with the chain (keeps POST response/event and registry GET/feed in sync).
            // The PT contract emits a keccak-derived (never all-zero) hash; an all-zero word means the
            // log carried none, so let the registry fall back to its locally computed hash.
            std::optional<evmc::bytes32> expected_format_hash;
            if(!chain::equalBytes32(event->format_hash, evmc::bytes32{}))
            {
                expected_format_hash = event->format_hash;
            }

            ConnectorRecord record = pt::buildConnectorRecordFromEvent(*event);
            co_return co_await _registry.addConnectorVersion(
                source, materializedAddress(event->connector_address), std::move(record), expected_format_hash);
        }

        // Unknown event type — cannot be materialized; the error log records the row
        // before projectBatch's retry budget poison-skips it.
        spdlog::error("[RegistryProjector] Unknown event_type='{}' at block={} log_index={} — will retry, then poison-skip",
            row.event_type, row.block_number, row.log_index);
        co_return false;
    }
}
