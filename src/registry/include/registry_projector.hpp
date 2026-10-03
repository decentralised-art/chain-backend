#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "native.h"
#include <asio.hpp>

#include "event_projector.hpp"
#include "sqlite_hot_store.hpp"
#include "registry.hpp"
#include "eth_client.hpp"

namespace dcn::registry
{
    /**
     * @brief Materialises finalised registry events from the hot-store changelog into the
     *        Registry database, using an independent change_seq cursor persisted in the
     *        registry DB.  Implements events::IEventProjector so it can be driven by the
     *        existing EventsRuntime projection loop.
     *
     * Cursor policy:
     *   - cursor is loaded from the registry DB at construction and persisted after each
     *     batch (only if at least one row was consumed or skipped).
     *   - non-finalized rows (observed/safe/removed) are skipped; their change_seq is
     *     still advanced past because they will re-surface with a higher change_seq when
     *     they reach "finalized".
     *
     * Failure policy (per _materializeOne):
     *   - success / already present → cursor advanced.
     *   - failure (undecodable log, divergence, add rejection) → cursor is left before
     *     the row and the row is retried on subsequent projectBatch passes, so transient
     *     failures (e.g. busy registry DB) heal on their own. After
     *     ProjectorRetryConfig::max_attempts consecutive failing passes the row is
     *     dead-lettered (REGISTRY_DEAD_LETTER_BIT in the hot store) and the cursor
     *     advances past it: a permanently parked cursor would freeze the prune
     *     watermark for every projector and grow the hot store without bound.
     *     Dead-lettered rows are exempt from pruning, so no chain event data is lost;
     *     a periodic idle-time sweep (ProjectorRetryConfig::sweep_interval_ms) retries
     *     them and clears the bit once materialization succeeds. If the dead-letter
     *     mark itself cannot be written, the cursor stays parked.
     *
     * Dependency back-fill: with a chain client, a chain connector whose transformations
     * or condition have no chain version in the registry (registered before the ingested
     * block range) gets them read from the PT registry state at the connector's block and
     * stored first, so the registry holds the connector's full dependency set.
     *
     * Runtime code: with a chain client, the deployed bytecode of every chain
     * transformation and condition is read from the chain at the block of its event and
     * stored in the registry under its code hash before the entity itself, unless the
     * registry already holds that code; a read failure retries the row.
     *
     * Creation code: with a chain client, the creation code every chain entity was deployed
     * from is read from the publish call in its event's transaction (the transaction's own
     * calldata, or the inner call a relay embeds) and stored by source and entity address,
     * so simulation can run its constructor. A transaction that cannot be read retries the
     * row; one whose calldata carries no publish call stores none. A back-filled dependency's
     * publication is located on chain; when it cannot be, it is kept without creation code.
     *
     * The code a batch needs is fetched up front with bounded concurrency, so a batch costs
     * a few round trips rather than one per entity, while rows still materialize in order.
     */
    class RegistryProjector final : public events::IEventProjector
    {
        public:
            RegistryProjector(
                events::SQLiteHotStore & store,
                Registry & registry,
                const asio::strand<asio::io_context::executor_type> & write_strand,
                events::ProjectorRetryConfig retry = {},
                std::shared_ptr<const rpc::EthClient> chain = nullptr);

            std::string_view id() const override;

            std::int64_t cursor() const override { return _cursor; }

            asio::awaitable<std::size_t> projectBatch(std::size_t limit, std::int64_t now_ms) override;

        private:
            // Returns true  → row materialized (or already present); advance past it.
            // Returns false → materialization failed; projectBatch retries the row on
            //                 later passes and dead-letters it after
            //                 _retry.max_attempts consecutive failures.
            asio::awaitable<bool> _materializeOne(const events::ChangeRecord & row);

            // Retries dead-lettered rows (throttled by _retry.sweep_interval_ms)
            // and clears their bit on success. Returns the number of rows resolved.
            asio::awaitable<std::size_t> _sweepDeadLetters(std::size_t limit, std::int64_t now_ms);

            // Stores the chain version of every transformation and condition `connector`
            // uses that the registry lacks. False when one of them cannot be read.
            asio::awaitable<bool> _backfillDependencies(
                const EntitySource & source,
                const std::string & block_hash,
                std::int64_t block_number,
                const Connector & connector);

            // Locates the publication of a back-filled entity, which has no event of ours, and
            // stores its creation code; called before the entity itself is stored, so a retry
            // of the connector's row recovers it again. False on a failure worth retrying: a
            // transient RPC error or a failed write. True once the code is stored, or when it
            // cannot be recovered at all (no publication found, e.g. on a node without
            // historical state, or no publish call in its transaction).
            asio::awaitable<bool> _storeBackfilledCreationCode(
                const EntitySource & source,
                EntityKind kind,
                const std::string & name,
                const chain::Address & entity,
                std::int64_t latest_block);

        public:
            // The chain code one entity's projection needs: its runtime code (transformations
            // and conditions) and the creation code its publication transaction carried.
            struct ChainCodeFetch
            {
                std::string source_id;
                chain::Address registry{};
                chain::Address entity{};
                std::string block_hash;
                std::string tx_hash;
                std::string runtime_code_hash;   // empty when the runtime code is not needed
                bool creation_code = false;
            };

            // What reads could not complete, by code hash and by entity address.
            struct ChainCodeMisses
            {
                std::set<std::string> runtime_code;       // unreadable runtime code
                std::set<std::string> creation_code;      // transaction could not be read
                std::set<std::string> no_creation_code;   // calldata carries no publish call

                void merge(ChainCodeMisses other);
            };

        private:
            // Stores the chain code `fetch` names that the registry lacks. False when a read
            // failed; true when the transaction carries no publish call. What this batch's
            // prefetch could not read is not read again.
            asio::awaitable<bool> _storeChainCode(ChainCodeFetch fetch);

            // Reads and stores the code `fetches` name. They share one publication transaction
            // and registry, so the transaction is read and searched once.
            asio::awaitable<ChainCodeMisses> _readChainCode(const std::vector<ChainCodeFetch> & fetches);

            // Stores the chain code `rows` need and the registry lacks, grouped by transaction
            // with at most CHAIN_CODE_FETCH_CONCURRENCY transactions in flight, and records what
            // could not be read in _misses. The first failed read stops new reads: rows are
            // materialized one by one, in order, and stop at the first that fails, so the
            // remaining reads would wait on a failing provider for nothing. Rows the prefetch
            // did not reach read their own code when they come up.
            asio::awaitable<void> _prefetchChainCode(const std::vector<events::ChangeRecord> & rows);

            // Reads each group in turn until `stop` is set, and sets it when a read fails.
            // Returns what could not be read.
            asio::awaitable<ChainCodeMisses> _fetchChainCode(
                std::vector<std::vector<ChainCodeFetch>> groups,
                std::atomic<bool> & stop);

            events::SQLiteHotStore & _store;
            Registry & _registry;
            asio::strand<asio::io_context::executor_type> _write_strand;
            events::ProjectorRetryConfig _retry;
            // ponytail: one client serves every chain source; key it by source_id when a
            // second RPC source is added.
            std::shared_ptr<const rpc::EthClient> _chain;
            std::int64_t _cursor{0};

            // Consecutive-failure tracking for the row currently at the cursor head.
            std::int64_t _failing_seq{0};
            std::size_t _failure_count{0};

            std::int64_t _last_sweep_ms{0};

            // What the current batch's prefetch could not read.
            ChainCodeMisses _misses;
    };
}
