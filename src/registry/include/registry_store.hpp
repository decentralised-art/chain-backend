#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "address.hpp"
#include "pt.hpp"
#include "format_hash.hpp"
#include "parser.hpp"
#include "sqlite/wal.hpp"

namespace dcn::registry
{
    using ScalarLabel = dcn::chain::ScalarLabel;
    using ConnectorRecordHandle = std::shared_ptr<const ConnectorRecord>;
    using TransformationRecordHandle = std::shared_ptr<const TransformationRecord>;
    using ConditionRecordHandle = std::shared_ptr<const ConditionRecord>;

    using EntityKind = pt::EntityKind;

    // source_id of the in-process EVM simulation; every other source is a chain.
    constexpr std::string_view LOCAL_SOURCE_ID = "local";

    struct EntitySource
    {
        std::string source_id = "local";
        std::string chain_namespace = "local";
        std::uint64_t chain_id = 1;
        std::string registry_address = "0x0";
        std::string content_hash;
        std::string metadata_hash;
        std::string runtime_code_hash;
        std::string block_hash;
        std::int64_t log_index = 0;
        pt::ArtifactState artifact_state = pt::ArtifactState::MISSING;
        bool definition_conflict = false;
    };

    struct EntityLocation
    {
        EntitySource source;
        chain::Address address{};
        bool active = true;
    };

    struct ArtifactReference
    {
        EntityKind kind = EntityKind::CONNECTOR;
        std::string source_id;
        std::string name;
        std::string content_hash;
        std::string metadata_hash;
        std::string runtime_code_hash;
        pt::ArtifactState state = pt::ArtifactState::MISSING;
        std::uint32_t attempts = 0;
    };

    struct ConnectorBatchItem
    {
        chain::Address address{};
        ConnectorRecord record;
        evmc::bytes32 format_hash{};
        std::vector<ScalarLabel> canonical_scalar_labels;
    };

    struct TransformationBatchItem
    {
        chain::Address address{};
        TransformationRecord record;
    };

    struct ConditionBatchItem
    {
        chain::Address address{};
        ConditionRecord record;
    };

    using NameCursor = std::string;

    inline std::string serializeNameCursor(const NameCursor & cursor)
    {
        return cursor;
    }

    struct NameCursorPage
    {
        std::vector<std::string> entries;
        std::optional<std::string> next_after;
        bool has_more = false;
    };

    class IRegistryStore
    {
        public:
            virtual ~IRegistryStore() = default;

            virtual bool hasConnector(const std::string & name) const = 0;

            virtual std::optional<ConnectorRecordHandle> getConnectorRecordHandle(const std::string & name) const = 0;

            virtual std::optional<evmc::bytes32> getConnectorFormatHash(const std::string & name) const = 0;

            virtual bool addConnector(
                const chain::Address & address,
                const ConnectorRecord & record,
                const evmc::bytes32 & format_hash,
                const std::vector<ScalarLabel> & canonical_scalar_labels) = 0;
            virtual bool addConnectorVersion(
                const EntitySource & source,
                const chain::Address & address,
                const ConnectorRecord & record,
                const evmc::bytes32 & format_hash,
                const std::vector<ScalarLabel> & canonical_scalar_labels) = 0;

            virtual bool addConnectorsBatch(
                const std::vector<ConnectorBatchItem> & items,
                bool all_or_nothing = true) = 0;

            virtual std::size_t getFormatConnectorNamesCount(const evmc::bytes32 & format_hash) const = 0;

            virtual NameCursorPage getFormatConnectorNamesCursor(
                const evmc::bytes32 & format_hash,
                const std::optional<NameCursor> & after,
                std::size_t limit) const = 0;

            virtual std::size_t getFormatsCount() const = 0;

            virtual NameCursorPage getFormatsCursor(
                const std::optional<evmc::bytes32> & after,
                std::size_t limit) const = 0;

            virtual std::optional<std::vector<ScalarLabel>> getScalarLabelsByFormatHash(
                const evmc::bytes32 & format_hash) const = 0;

            virtual bool hasTransformation(const std::string & name) const = 0;

            virtual std::optional<TransformationRecordHandle> getTransformationRecordHandle(
                const std::string & name) const = 0;

            virtual bool addTransformation(
                const chain::Address & address,
                const TransformationRecord & record) = 0;
            virtual bool addTransformationVersion(
                const EntitySource & source,
                const chain::Address & address,
                const TransformationRecord & record) = 0;

            virtual bool addTransformationsBatch(
                const std::vector<TransformationBatchItem> & items,
                bool all_or_nothing = true) = 0;

            virtual bool hasCondition(const std::string & name) const = 0;
            virtual std::optional<ConditionRecordHandle> getConditionRecordHandle(
                const std::string & name) const = 0;

            virtual bool addCondition(
                const chain::Address & address,
                const ConditionRecord & record) = 0;
            virtual bool addConditionVersion(
                const EntitySource & source,
                const chain::Address & address,
                const ConditionRecord & record) = 0;

            virtual std::optional<EntityLocation> getEntityLocation(EntityKind kind, const std::string & name) const = 0;
            virtual bool hasRemoteEntity(EntityKind kind, const std::string & name) const = 0;
            // True when this server's local simulation holds a version of the entity.
            virtual bool hasLocalEntity(EntityKind kind, const std::string & name) const = 0;
            virtual bool removeEntityVersion(EntityKind kind, const std::string & source_id, const std::string & name) = 0;
            virtual bool changeEntityOwner(
                EntityKind kind,
                const std::string & source_id,
                const std::string & name,
                const chain::Address & old_owner,
                const chain::Address & new_owner) = 0;
            virtual std::vector<ArtifactReference> getArtifactsDue(
                std::int64_t now_ms,
                std::size_t limit) const = 0;
            virtual bool setArtifactState(
                EntityKind kind,
                const std::string & source_id,
                const std::string & name,
                pt::ArtifactState state,
                std::int64_t retry_at_ms) = 0;

            virtual bool addConditionsBatch(
                const std::vector<ConditionBatchItem> & items,
                bool all_or_nothing = true) = 0;

            virtual NameCursorPage getOwnedConnectorsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const = 0;

            virtual NameCursorPage getOwnedTransformationsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const = 0;

            virtual NameCursorPage getOwnedConditionsCursor(
                const chain::Address & owner,
                const std::optional<NameCursor> & after,
                std::size_t limit) const = 0;

            virtual std::size_t getAccountsCount() const = 0;

            virtual NameCursorPage getAccountsCursor(
                const std::optional<chain::Address> & after,
                std::size_t limit) const = 0;

            virtual bool checkpointWal(storage::sqlite::WalCheckpointMode mode) const = 0;

            virtual std::int64_t getMaterializationCursor() const = 0;
            virtual bool setMaterializationCursor(std::int64_t last_change_seq) = 0;
    };
}

namespace dcn::parse
{
    Result<registry::NameCursor> parseNameCursor(const std::string & name_token);
    Result<evmc::bytes32> parseFormatCursorHex(const std::string & cursor_token);
}
