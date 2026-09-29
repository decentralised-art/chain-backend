#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <spdlog/spdlog.h>

#include "async.hpp"
#include "pt.hpp"
#include "address.hpp"
#include "format_hash.hpp"
#include "registry_store.hpp"
#include "sqlite_registry_store.hpp"
#include "sqlite/wal_store.hpp"

namespace dcn::registry
{
    using ScalarLabel = dcn::chain::ScalarLabel;

    template<typename KeyT, typename ValueT>
    struct LruCache
    {
        struct Entry
        {
            ValueT value;
            typename std::list<KeyT>::iterator order_it;
        };

        const char * name = "unnamed-cache";
        std::size_t capacity = 0;
        std::list<KeyT> order;
        absl::flat_hash_map<KeyT, Entry> entries;
    };

    class Registry : public storage::sqlite::IWalStore
    {
        public:
            Registry() = delete;
            Registry(asio::io_context & io_context, std::string sqlite_path = ":memory:");

            Registry(const Registry&) = delete;
            Registry& operator=(const Registry&) = delete;

            ~Registry() = default;

            // expected_format_hash, when set, is the chain-emitted format hash. The registry is a
            // mirror of chain state, so a locally-recomputed hash that disagrees with the chain is a
            // divergence: addConnector refuses rather than storing a hash the POST response/event
            // never carried.
            asio::awaitable<bool> addConnector(chain::Address address, ConnectorRecord connector,
                std::optional<evmc::bytes32> expected_format_hash = std::nullopt);
            asio::awaitable<bool> addConnectorVersion(
                EntitySource source,
                chain::Address address,
                ConnectorRecord connector,
                std::optional<evmc::bytes32> expected_format_hash = std::nullopt);
            asio::awaitable<bool> addConnectorsBatch(
                std::vector<std::pair<chain::Address, ConnectorRecord>> connectors,
                bool all_or_nothing = true);

            // Deterministically compute a connector's composite-aware format hash from its
            // definition, without persisting it. Resolves composite dimensions against
            // already-registered connectors; returns nullopt if resolution/validation fails.
            asio::awaitable<std::optional<evmc::bytes32>> computeConnectorFormatHash(Connector connector) const;

            asio::awaitable<std::optional<ConnectorRecordHandle>> getConnectorRecordHandle(
                const std::string & name) const;

            asio::awaitable<bool> hasConnector(const std::string & name) const;

            asio::awaitable<std::optional<evmc::bytes32>> getFormatHash(const std::string& name) const;

            asio::awaitable<std::size_t> getFormatConnectorNamesCount(const evmc::bytes32 & format_hash) const;

            asio::awaitable<NameCursorPage> getFormatConnectorNamesCursor(
                const evmc::bytes32 & format_hash,
                const std::optional<NameCursor> & after,
                std::size_t limit) const;

            asio::awaitable<std::size_t> getFormatsCount() const;

            asio::awaitable<NameCursorPage> getFormatsCursor(
                const std::optional<evmc::bytes32> & after,
                std::size_t limit) const;

            asio::awaitable<std::optional<std::vector<ScalarLabel>>> getScalarLabelsByFormatHash(const evmc::bytes32 & format_hash) const;

            asio::awaitable<bool> addTransformation(chain::Address address, TransformationRecord transformation);
            asio::awaitable<bool> addTransformationVersion(
                EntitySource source,
                chain::Address address,
                TransformationRecord transformation);
            asio::awaitable<bool> addTransformationsBatch(
                std::vector<std::pair<chain::Address, TransformationRecord>> transformations,
                bool all_or_nothing = true);

            asio::awaitable<std::optional<TransformationRecordHandle>> getTransformationRecordHandle(
                const std::string & name) const;

            asio::awaitable<bool> hasTransformation(const std::string & name) const;

            asio::awaitable<bool> addCondition(chain::Address address, ConditionRecord condition);
            asio::awaitable<bool> addConditionVersion(
                EntitySource source,
                chain::Address address,
                ConditionRecord condition);

            asio::awaitable<std::optional<EntityLocation>> getEntityLocation(EntityKind kind, const std::string & name) const;
            asio::awaitable<bool> hasRemoteEntity(EntityKind kind, const std::string & name) const;
            asio::awaitable<bool> hasLocalEntity(EntityKind kind, const std::string & name) const;
            asio::awaitable<bool> removeEntityVersion(EntityKind kind, std::string source_id, std::string name);
            asio::awaitable<bool> changeEntityOwner(
                EntityKind kind,
                std::string source_id,
                std::string name,
                chain::Address old_owner,
                chain::Address new_owner);
            asio::awaitable<std::vector<ArtifactReference>> getArtifactsDue(
                std::int64_t now_ms,
                std::size_t limit) const;
            asio::awaitable<bool> setArtifactState(
                EntityKind kind,
                std::string source_id,
                std::string name,
                pt::ArtifactState state,
                std::int64_t retry_at_ms);

            asio::awaitable<bool> addConditionsBatch(
                std::vector<std::pair<chain::Address, ConditionRecord>> conditions,
                bool all_or_nothing = true);

            asio::awaitable<std::optional<ConditionRecordHandle>> getConditionRecordHandle(
                const std::string & name) const;

            asio::awaitable<bool> hasCondition(const std::string & name) const;

            asio::awaitable<NameCursorPage> getOwnedConnectorsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const;

            asio::awaitable<NameCursorPage> getOwnedTransformationsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const;

            asio::awaitable<NameCursorPage> getOwnedConditionsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const;

            asio::awaitable<std::size_t> getAccountsCount() const;

            asio::awaitable<NameCursorPage> getAccountsCursor(
                const std::optional<chain::Address> & after,
                std::size_t limit) const;

            asio::awaitable<storage::sqlite::WalCheckpointStats> checkpointWal(storage::sqlite::WalCheckpointMode mode) const override;

            // Materialization cursor, owned by RegistryProjector. getMaterializationCursor
            // is synchronous because it is read once at construction (pre-start, single
            // threaded). setMaterializationCursor runs on the Registry strand so the cursor
            // write serializes with every other access to the registry DB.
            std::int64_t getMaterializationCursor() const;
            asio::awaitable<bool> setMaterializationCursor(std::int64_t seq);

        private:
            static constexpr std::size_t kHotCacheCapacity = 1024;

            asio::strand<asio::io_context::executor_type> _strand;
            std::unique_ptr<IRegistryStore> _store;
            mutable LruCache<std::string, std::optional<ConnectorRecordHandle>> _connector_record_cache;
            mutable LruCache<std::string, std::optional<evmc::bytes32>> _format_hash_cache;
            mutable LruCache<std::string, std::optional<TransformationRecordHandle>> _transformation_record_cache;
            mutable LruCache<std::string, std::optional<ConditionRecordHandle>> _condition_record_cache;
    };
}


