#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

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
                const Connector & connector);

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
    };
}
