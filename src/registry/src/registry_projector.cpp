#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <asio/experimental/parallel_group.hpp>
#include <spdlog/spdlog.h>

#include "registry_projector.hpp"

#include "connector.hpp"
#include "transformation.hpp"
#include "condition.hpp"
#include "publication.hpp"
#include "address.hpp"
#include "chain.hpp"
#include "format_hash.hpp"
#include "hex.hpp"
#include "async.hpp"

namespace dcn::registry
{
    namespace
    {
        // Entities whose chain code one projection batch reads at once.
        constexpr std::size_t CHAIN_CODE_FETCH_CONCURRENCY = 8;

        // The row's topics in order, up to the first absent one.
        std::vector<std::string> rowTopics(const events::ChangeRecord & row)
        {
            std::vector<std::string> topics;
            for(const auto & topic : row.topics)
            {
                if(!topic.has_value()) break;
                topics.push_back(*topic);
            }
            return topics;
        }

        // The chain code the Added event in `row` needs: runtime code for a transformation
        // or condition, and the creation code of its publication for every kind. nullopt for
        // any other row or an undecodable one.
        std::optional<RegistryProjector::ChainCodeFetch> chainCodeFetchFor(const events::ChangeRecord & row)
        {
            const auto registry = evmc::from_hex<chain::Address>(row.registry_address);
            if(!registry) return std::nullopt;
            RegistryProjector::ChainCodeFetch fetch{
                .source_id = row.source_id,
                .registry = *registry,
                .block_hash = row.block_hash,
                .tx_hash = row.tx_hash,
                .creation_code = true};

            // The row's runtime code hash is the one the entity's registry version records.
            const auto fill = [&](const chain::Address & entity, const bool runtime_code)
            {
                fetch.entity = entity;
                if(runtime_code && row.runtime_code_hash) fetch.runtime_code_hash = *row.runtime_code_hash;
            };

            const std::vector<std::string> topics = rowTopics(row);
            if(row.event_type == events::TRANSFORMATION_ADDED_TYPE)
            {
                const auto event = pt::decodeTransformationAddedEvent(row.data_hex, topics);
                if(!event) return std::nullopt;
                fill(event->transformation_address, true);
                return fetch;
            }
            if(row.event_type == events::CONDITION_ADDED_TYPE)
            {
                const auto event = pt::decodeConditionAddedEvent(row.data_hex, topics);
                if(!event) return std::nullopt;
                fill(event->condition_address, true);
                return fetch;
            }
            if(row.event_type == events::CONNECTOR_ADDED_TYPE)
            {
                const auto event = pt::decodeConnectorAddedEvent(row.data_hex, topics);
                if(!event) return std::nullopt;
                fill(event->connector_address, false);
                return fetch;
            }
            return std::nullopt;
        }

        std::string encodeCall(const std::string & signature, const std::vector<pt::abi::Component> & arguments)
        {
            std::vector<std::uint8_t> call = chain::constructSelector(signature);
            const std::vector<std::uint8_t> encoded = pt::abi::encodeTuple(arguments);
            call.insert(call.end(), encoded.begin(), encoded.end());
            return chain::bytesToHex(call);
        }

        std::optional<std::string> bytes32FromWord(const std::string & word_hex)
        {
            const auto value = evmc::from_hex<evmc::bytes32>(word_hex);
            if(!value) return std::nullopt;
            return chain::bytes32ToHex(*value);
        }

        struct ChainEntityState
        {
            chain::Address address{};
            std::string owner;
            std::uint32_t args_count = 0;
            std::string content_hash;
            std::string metadata_hash;
            std::string runtime_code_hash;
            std::vector<std::uint8_t> runtime_code;
        };

        // The entity's runtime bytecode at the block with `block_hash`; nullopt when it cannot
        // be read or is empty.
        asio::awaitable<std::optional<std::vector<std::uint8_t>>> readRuntimeCode(
            const rpc::EthClient & client,
            const std::string & address_hex,
            const std::string & block_hash)
        {
            const auto code_hex = co_await client.getCodeAtBlock(address_hex, block_hash);
            if(!code_hex) co_return std::nullopt;
            const auto code = evmc::from_hex(*code_hex);
            if(!code || code->empty()) co_return std::nullopt;
            co_return std::vector<std::uint8_t>(code->begin(), code->end());
        }

        // A transformation or condition as the PT registry holds it at `block_hash`: the
        // fields its Added event carries. The runtime code hash is the entity's code hash,
        // which the registry checks against the publication when it deploys the entity.
        asio::awaitable<std::optional<ChainEntityState>> readChainEntity(
            const rpc::EthClient & client,
            const std::string & registry_address,
            const std::string & block_hash,
            const EntityKind kind,
            const std::string & name)
        {
            const std::vector<pt::abi::Component> entity_key{
                pt::abi::uintComponent(static_cast<std::uint64_t>(kind)),
                pt::abi::stringComponent(name)};

            // GCC 13 ICEs on braced-init temporaries inside co_await expressions, so each
            // call payload is built before it is awaited.
            const std::vector<pt::abi::Component> name_key{pt::abi::stringComponent(name)};
            const std::string address_call = encodeCall(
                kind == EntityKind::TRANSFORMATION ? "getTransformation(string)" : "getCondition(string)", name_key);
            const std::string owner_call = encodeCall("getEntityOwner(uint8,string)", entity_key);
            const std::string content_call = encodeCall("getEntityContentHash(uint8,string)", entity_key);
            const std::string metadata_call = encodeCall("getEntityMetadataHash(uint8,string)", entity_key);

            const auto address_word = co_await client.callAtBlock(registry_address, address_call, block_hash);
            const auto owner_word = co_await client.callAtBlock(registry_address, owner_call, block_hash);
            const auto content_word = co_await client.callAtBlock(registry_address, content_call, block_hash);
            const auto metadata_word = co_await client.callAtBlock(registry_address, metadata_call, block_hash);
            if(!address_word || !owner_word || !content_word || !metadata_word)
            {
                spdlog::error("[RegistryProjector] cannot read {} '{}' from registry {} at block {}",
                    pt::entityKindName(kind), name, registry_address, block_hash);
                co_return std::nullopt;
            }

            const auto entity_hex = rpc::addressFromWord(*address_word, "entity address");
            const auto owner_hex = rpc::addressFromWord(*owner_word, "entity owner");
            const auto content_hash = bytes32FromWord(*content_word);
            const auto metadata_hash = bytes32FromWord(*metadata_word);
            if(!entity_hex || !owner_hex || !content_hash || !metadata_hash)
            {
                spdlog::error("[RegistryProjector] registry {} returned malformed state for {} '{}'",
                    registry_address, pt::entityKindName(kind), name);
                co_return std::nullopt;
            }

            const std::string args_call = encodeCall("getArgsCount()", std::vector<pt::abi::Component>{});
            const auto args_word = co_await client.callAtBlock(*entity_hex, args_call, block_hash);
            auto runtime_code = co_await readRuntimeCode(client, *entity_hex, block_hash);
            std::optional<std::uint64_t> args_count;
            if(args_word)
            {
                if(const auto parsed = rpc::uint64FromWord(*args_word, "args count")) args_count = *parsed;
            }
            if(!args_count || *args_count > std::numeric_limits<std::uint32_t>::max() || !runtime_code)
            {
                spdlog::error("[RegistryProjector] cannot read {} '{}' at {}",
                    pt::entityKindName(kind), name, *entity_hex);
                co_return std::nullopt;
            }

            ChainEntityState state{
                .address = evmc::from_hex<chain::Address>(*entity_hex).value_or(chain::Address{}),
                .owner = *owner_hex,
                .args_count = static_cast<std::uint32_t>(*args_count),
                .content_hash = *content_hash,
                .metadata_hash = *metadata_hash,
                .runtime_code_hash = chain::bytes32ToHex(chain::keccakBytes(runtime_code->data(), runtime_code->size())),
                .runtime_code = std::move(*runtime_code)};
            co_return state;
        }
    }

    RegistryProjector::RegistryProjector(
        events::SQLiteHotStore & store,
        Registry & registry,
        const asio::strand<asio::io_context::executor_type> & write_strand,
        events::ProjectorRetryConfig retry,
        std::shared_ptr<const rpc::EthClient> chain)
        : _store(store)
        , _registry(registry)
        , _write_strand(write_strand)
        , _retry(retry)
        , _chain(std::move(chain))
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
        co_await _prefetchChainCode(rows);
        co_await async::ensureOnStrand(_write_strand);
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
        co_await _prefetchChainCode(rows);

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

    asio::awaitable<bool> RegistryProjector::_backfillDependencies(
        const EntitySource & source,
        const std::string & block_hash,
        const std::int64_t block_number,
        const Connector & connector)
    {
        if(!_chain) co_return true;

        std::set<std::pair<EntityKind, std::string>> dependencies;
        for(const auto & dimension : connector.dimensions())
        {
            for(const auto & transformation : dimension.transformations())
            {
                if(!transformation.name().empty())
                {
                    dependencies.emplace(EntityKind::TRANSFORMATION, transformation.name());
                }
            }
        }
        if(!connector.condition_name().empty())
        {
            dependencies.emplace(EntityKind::CONDITION, connector.condition_name());
        }

        // ponytail: composite and binding connectors are not back-filled; their definition
        // lives only in their event, not in readable registry state.
        for(const auto & [kind, name] : dependencies)
        {
            if(co_await _registry.hasRemoteEntity(kind, name)) continue;

            const auto state = co_await readChainEntity(*_chain, source.registry_address, block_hash, kind, name);
            if(!state) co_return false;
            if(!co_await _registry.putRuntimeCode(state->runtime_code_hash, state->runtime_code)) co_return false;
            if(!co_await _storeBackfilledCreationCode(source, kind, name, state->address, block_number)) co_return false;

            // Read from registry state rather than an event: the origin is the block it was
            // read at, with no log index.
            EntitySource dependency_source = source;
            dependency_source.content_hash = state->content_hash;
            dependency_source.metadata_hash = state->metadata_hash;
            dependency_source.runtime_code_hash = state->runtime_code_hash;
            dependency_source.log_index = -1;
            dependency_source.definition_conflict = false;

            bool added = false;
            if(kind == EntityKind::TRANSFORMATION)
            {
                TransformationRecord record;
                record.mutable_transformation()->set_name(name);
                record.mutable_transformation()->set_args_count(state->args_count);
                record.set_owner(state->owner);
                added = co_await _registry.addTransformationVersion(dependency_source, state->address, std::move(record));
            }
            else
            {
                ConditionRecord record;
                record.mutable_condition()->set_name(name);
                record.mutable_condition()->set_args_count(state->args_count);
                record.set_owner(state->owner);
                added = co_await _registry.addConditionVersion(dependency_source, state->address, std::move(record));
            }
            if(!added) co_return false;

            spdlog::info("[RegistryProjector] back-filled {} '{}' of connector '{}' from source '{}'",
                pt::entityKindName(kind), name, connector.name(), source.source_id);
        }
        co_return true;
    }

    void RegistryProjector::ChainCodeMisses::merge(ChainCodeMisses other)
    {
        runtime_code.merge(other.runtime_code);
        creation_code.merge(other.creation_code);
        no_creation_code.merge(other.no_creation_code);
    }

    asio::awaitable<bool> RegistryProjector::_storeBackfilledCreationCode(
        const EntitySource & source,
        const EntityKind kind,
        const std::string & name,
        const chain::Address & entity,
        const std::int64_t latest_block)
    {
        if(co_await _registry.hasChainCreationCode(source.source_id, entity)) co_return true;

        // A transient provider failure is retried with the connector's row; any other answer
        // is final and leaves the entity without creation code.
        const auto unrecoverable = [&](const std::string_view reason)
        {
            spdlog::warn("[RegistryProjector] the creation code of back-filled {} '{}' cannot be recovered ({}); "
                "it is kept without it and cannot be simulated", pt::entityKindName(kind), name, reason);
            return true;
        };
        const auto failed = [&](const rpc::RpcError & error)
        {
            if(!error.retryable() && error.kind != rpc::RpcError::Kind::QUOTA) return unrecoverable(error.message);
            spdlog::error("[RegistryProjector] cannot recover the creation code of back-filled {} '{}': {}",
                pt::entityKindName(kind), name, error.message);
            return false;
        };

        const auto registry = evmc::from_hex<chain::Address>(source.registry_address);
        if(!registry) co_return unrecoverable("the registry address is malformed");
        const auto location = co_await rpc::findEntityPublication(
            *_chain, source.registry_address, kind, name, entity, latest_block);
        if(!location) co_return failed(location.error());
        if(!*location) co_return unrecoverable("no matching publication event");

        auto codes = co_await rpc::readPublishedCreationCodes(*_chain, (**location).tx_hash, (**location).block_hash, *registry);
        if(!codes) co_return failed(codes.error());
        const auto code = codes->find(entity);
        if(code == codes->end()) co_return unrecoverable("its transaction carries no readable publish call");

        if(!co_await _registry.putChainCreationCode(source.source_id, entity, std::move(code->second)))
        {
            spdlog::error("[RegistryProjector] cannot store the creation code of back-filled {} '{}'",
                pt::entityKindName(kind), name);
            co_return false;
        }
        co_return true;
    }

    asio::awaitable<RegistryProjector::ChainCodeMisses> RegistryProjector::_readChainCode(const std::vector<ChainCodeFetch> & fetches)
    {
        ChainCodeMisses misses;
        for(const auto & fetch : fetches)
        {
            if(fetch.runtime_code_hash.empty()) continue;
            const std::string entity_hex = chain::addressToHex(fetch.entity);
            auto code = co_await readRuntimeCode(*_chain, entity_hex, fetch.block_hash);
            if(!code || !co_await _registry.putRuntimeCode(fetch.runtime_code_hash, std::move(*code)))
            {
                spdlog::error("[RegistryProjector] cannot read runtime code of {}", entity_hex);
                misses.runtime_code.insert(fetch.runtime_code_hash);
            }
        }

        const auto first = std::ranges::find_if(fetches, &ChainCodeFetch::creation_code);
        if(first == fetches.end()) co_return misses;
        auto codes = co_await rpc::readPublishedCreationCodes(*_chain, first->tx_hash, first->block_hash, first->registry);
        if(!codes)
        {
            spdlog::error("[RegistryProjector] cannot read publication transaction {}: {}",
                first->tx_hash, codes.error().message);
        }

        for(const auto & fetch : fetches)
        {
            if(!fetch.creation_code) continue;
            const std::string entity_hex = chain::addressToHex(fetch.entity);
            if(!codes)
            {
                misses.creation_code.insert(entity_hex);
                continue;
            }
            const auto code = codes->find(fetch.entity);
            if(code == codes->end())
            {
                spdlog::warn("[RegistryProjector] transaction {} carries no readable publish call for {}; "
                    "it cannot be simulated", fetch.tx_hash, entity_hex);
                misses.no_creation_code.insert(entity_hex);
            }
            else if(!co_await _registry.putChainCreationCode(fetch.source_id, fetch.entity, std::move(code->second)))
            {
                misses.creation_code.insert(entity_hex);
            }
        }
        co_return misses;
    }

    asio::awaitable<bool> RegistryProjector::_storeChainCode(ChainCodeFetch fetch)
    {
        if(!_chain) co_return true;
        const std::string entity_hex = chain::addressToHex(fetch.entity);

        if(!fetch.runtime_code_hash.empty() && co_await _registry.hasRuntimeCode(fetch.runtime_code_hash))
        {
            fetch.runtime_code_hash.clear();
        }
        if(fetch.creation_code && (_misses.no_creation_code.contains(entity_hex)
                                   || co_await _registry.hasChainCreationCode(fetch.source_id, fetch.entity)))
        {
            fetch.creation_code = false;
        }
        if((!fetch.runtime_code_hash.empty() && _misses.runtime_code.contains(fetch.runtime_code_hash))
           || (fetch.creation_code && _misses.creation_code.contains(entity_hex)))
        {
            spdlog::error("[RegistryProjector] chain code of {} could not be read in this batch", entity_hex);
            co_return false;
        }
        if(fetch.runtime_code_hash.empty() && !fetch.creation_code) co_return true;

        // Built before the co_await: GCC 13 ICEs on braced-init temporaries inside one.
        const std::vector<ChainCodeFetch> single{std::move(fetch)};
        const ChainCodeMisses misses = co_await _readChainCode(single);
        co_return misses.runtime_code.empty() && misses.creation_code.empty();
    }

    asio::awaitable<void> RegistryProjector::_prefetchChainCode(const std::vector<events::ChangeRecord> & rows)
    {
        _misses = {};
        if(!_chain) co_return;

        // Fetches grouped by the transaction and registry that published them, in row order.
        std::vector<std::vector<ChainCodeFetch>> groups;
        std::map<std::pair<std::string, std::string>, std::size_t> group_of;
        std::set<std::string> planned;
        for(const auto & row : rows)
        {
            if(row.state != events::FINALIZED_STATE || row.source_id == events::LOCAL_SOURCE_ID) continue;
            auto fetch = chainCodeFetchFor(row);
            if(!fetch || !planned.insert(chain::addressToHex(fetch->entity)).second) continue;

            if(!fetch->runtime_code_hash.empty() && co_await _registry.hasRuntimeCode(fetch->runtime_code_hash))
            {
                fetch->runtime_code_hash.clear();
            }
            if(co_await _registry.hasChainCreationCode(fetch->source_id, fetch->entity))
            {
                fetch->creation_code = false;
            }
            if(fetch->runtime_code_hash.empty() && !fetch->creation_code) continue;

            const auto key = std::pair(chain::normalizeHex(fetch->tx_hash), chain::addressToHex(fetch->registry));
            const auto [group, added] = group_of.try_emplace(key, groups.size());
            if(added) groups.emplace_back();
            groups[group->second].push_back(std::move(*fetch));
        }
        if(groups.empty()) co_return;

        // Worker i takes groups i, i+K, i+2K...: at most K transactions are in flight and
        // the workers share only the stop flag.
        std::atomic<bool> stop{false};
        const auto executor = co_await asio::this_coro::executor;
        using Worker = decltype(asio::co_spawn(executor, _fetchChainCode({}, stop), asio::deferred));
        std::vector<Worker> workers;
        const std::size_t worker_count = std::min(CHAIN_CODE_FETCH_CONCURRENCY, groups.size());
        for(std::size_t first = 0; first < worker_count; ++first)
        {
            std::vector<std::vector<ChainCodeFetch>> share;
            for(std::size_t i = first; i < groups.size(); i += worker_count) share.push_back(groups[i]);
            workers.push_back(asio::co_spawn(executor, _fetchChainCode(std::move(share), stop), asio::deferred));
        }

        auto [order, exceptions, misses] = co_await asio::experimental::make_parallel_group(std::move(workers))
            .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
        (void)order;
        for(std::size_t i = 0; i < misses.size(); ++i)
        {
            // A worker that failed outright leaves its rows to their own reads in order.
            if(exceptions[i]) continue;
            _misses.merge(std::move(misses[i]));
        }
    }

    asio::awaitable<RegistryProjector::ChainCodeMisses> RegistryProjector::_fetchChainCode(
        std::vector<std::vector<ChainCodeFetch>> groups,
        std::atomic<bool> & stop)
    {
        ChainCodeMisses misses;
        for(const auto & group : groups)
        {
            if(stop) break;
            ChainCodeMisses read = co_await _readChainCode(group);
            if(!read.runtime_code.empty() || !read.creation_code.empty()) stop = true;
            misses.merge(std::move(read));
        }
        co_return misses;
    }

    asio::awaitable<bool> RegistryProjector::_materializeOne(const events::ChangeRecord & row)
    {
        const std::vector<std::string> topics = rowTopics(row);

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

        // A chain version whose content differs from the version currently resolved for
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
                    // Already present with the same definition; a rebuild may have changed its code.
                    co_return co_await _registry.updateLocalVersionHashes(EntityKind::TRANSFORMATION, event->name, source);
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

            const std::optional<ChainCodeFetch> chain_code = local_source ? std::nullopt : chainCodeFetchFor(row);
            if(chain_code && !co_await _storeChainCode(*chain_code))
            {
                co_return false;
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
                    // Already present with the same definition; a rebuild may have changed its code.
                    co_return co_await _registry.updateLocalVersionHashes(EntityKind::CONDITION, event->name, source);
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

            const std::optional<ChainCodeFetch> chain_code = local_source ? std::nullopt : chainCodeFetchFor(row);
            if(chain_code && !co_await _storeChainCode(*chain_code))
            {
                co_return false;
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
                    // Already present with the same definition; a rebuild may have changed its code.
                    co_return co_await _registry.updateLocalVersionHashes(EntityKind::CONNECTOR, event->name, source);
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
            const std::optional<ChainCodeFetch> chain_code = local_source ? std::nullopt : chainCodeFetchFor(row);
            if(!local_source && !co_await _backfillDependencies(source, row.block_hash, row.block_number, record.connector()))
            {
                co_return false;
            }
            if(chain_code && !co_await _storeChainCode(*chain_code))
            {
                co_return false;
            }
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
