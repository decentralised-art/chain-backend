#pragma once

#include <optional>
#include <string>
#include <vector>

#include "registry_store.hpp"

struct sqlite3;

namespace dcn::registry
{
    class SQLiteRegistryStore final : public IRegistryStore
    {
        public:
            explicit SQLiteRegistryStore(const std::string & db_path);
            ~SQLiteRegistryStore() override;

            SQLiteRegistryStore(const SQLiteRegistryStore &) = delete;
            SQLiteRegistryStore & operator=(const SQLiteRegistryStore &) = delete;

            SQLiteRegistryStore(SQLiteRegistryStore &&) = delete;
            SQLiteRegistryStore & operator=(SQLiteRegistryStore &&) = delete;

            bool hasConnector(const std::string & name) const override;

            std::optional<ConnectorRecordHandle> getConnectorRecordHandle(const std::string & name) const override;

            std::optional<evmc::bytes32> getConnectorFormatHash(const std::string & name) const override;

            bool addConnector(
                const chain::Address & address,
                const ConnectorRecord & record,
                const evmc::bytes32 & format_hash,
                const std::vector<ScalarLabel> & canonical_scalar_labels) override;
            bool addConnectorVersion(
                const EntitySource & source,
                const chain::Address & address,
                const ConnectorRecord & record,
                const evmc::bytes32 & format_hash,
                const std::vector<ScalarLabel> & canonical_scalar_labels) override;

            bool addConnectorsBatch(const std::vector<ConnectorBatchItem> & items, bool all_or_nothing = true) override;

            std::size_t getFormatConnectorNamesCount(const evmc::bytes32 & format_hash) const override;

            NameCursorPage getFormatConnectorNamesCursor(
                const evmc::bytes32 & format_hash,
                const std::optional<NameCursor> & after,
                std::size_t limit) const override;

            std::size_t getFormatsCount() const override;

            NameCursorPage getFormatsCursor(
                const std::optional<evmc::bytes32> & after,
                std::size_t limit) const override;

            std::optional<std::vector<ScalarLabel>> getScalarLabelsByFormatHash(const evmc::bytes32 & format_hash) const override;

            bool hasTransformation(const std::string & name) const override;

            std::optional<TransformationRecordHandle> getTransformationRecordHandle(
                const std::string & name) const override;

            bool addTransformation(
                const chain::Address & address,
                const TransformationRecord & record) override;
            bool addTransformationVersion(
                const EntitySource & source,
                const chain::Address & address,
                const TransformationRecord & record) override;

            bool addTransformationsBatch(
                const std::vector<TransformationBatchItem> & items,
                bool all_or_nothing = true) override;

            bool hasCondition(const std::string & name) const override;

            std::optional<ConditionRecordHandle> getConditionRecordHandle(
                const std::string & name) const override;

            bool addCondition(
                const chain::Address & address,
                const ConditionRecord & record) override;
            bool addConditionVersion(
                const EntitySource & source,
                const chain::Address & address,
                const ConditionRecord & record) override;

            std::optional<EntityLocation> getEntityLocation(EntityKind kind, const std::string & name) const override;
            bool hasRemoteEntity(EntityKind kind, const std::string & name) const override;
            bool hasLocalEntity(EntityKind kind, const std::string & name) const override;
            bool removeEntityVersion(EntityKind kind, const std::string & source_id, const std::string & name) override;
            bool changeEntityOwner(
                EntityKind kind,
                const std::string & source_id,
                const std::string & name,
                const chain::Address & old_owner,
                const chain::Address & new_owner) override;
            std::vector<ArtifactReference> getArtifactsDue(
                std::int64_t now_ms,
                std::size_t limit) const override;
            bool setArtifactState(
                EntityKind kind,
                const std::string & source_id,
                const std::string & name,
                pt::ArtifactState state,
                std::int64_t retry_at_ms) override;

            bool addConditionsBatch(
                const std::vector<ConditionBatchItem> & items,
                bool all_or_nothing = true) override;

            NameCursorPage getOwnedConnectorsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const override;

            NameCursorPage getOwnedTransformationsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const override;

            NameCursorPage getOwnedConditionsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const override;

            std::size_t getAccountsCount() const override;

            NameCursorPage getAccountsCursor(
                const std::optional<chain::Address> & after,
                std::size_t limit) const override;

            bool checkpointWal(storage::sqlite::WalCheckpointMode mode) const override;

            // Materialization cursor: persists the last change_seq consumed from the
            // hot-store changelog, so RegistryProjector survives restarts.
            std::int64_t getMaterializationCursor() const override;
            bool setMaterializationCursor(std::int64_t last_change_seq) override;

        private:
            sqlite3 * _db = nullptr;

            bool _initializeSchema() const;
            bool _exec(const char * sql) const;
            bool _beginTransaction() const;
            bool _commitTransaction() const;
            void _rollbackTransaction() const;

            // Persists a connector and its normalized child rows (dimensions, bindings,
            // transformation defs + args, condition args, static running instances) using explicit
            // columns. Must be called inside an active transaction/savepoint.
            bool _insertConnectorRows(
                const EntitySource & source,
                const chain::Address & address,
                const chain::Address & owner,
                const evmc::bytes32 & format_hash,
                const ConnectorRecord & record,
                const std::vector<ScalarLabel> & canonical_scalar_labels) const;

            // Reassembles a ConnectorRecord from its explicit columns / child tables.
            std::optional<ConnectorRecord> _readConnectorRecord(const std::string & source_id, const std::string & name) const;

            NameCursorPage _getOwnedCursorFromTable(
                const char * table_name,
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const;
    };
}
