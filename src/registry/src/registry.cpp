#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <evmc/hex.hpp>

#include "registry.hpp"
#include "sqlite_registry_store.hpp"
#include "transformation.hpp"
#include "condition.hpp"

namespace dcn::registry
{
    namespace
    {
        enum class VisitState : std::uint8_t
        {
            VISITING,
            DONE
        };

        // Shared connector graph lookups used while resolving recursive dependencies.
        struct ConnectorGraphContext
        {
            const Connector & pending_connector;
            const IRegistryStore & store;
            const absl::flat_hash_map<std::string, Connector> * staged_connectors = nullptr;
            absl::flat_hash_map<std::string, Connector> connector_cache;
        };

        // Resolve connector by name from pending connector + persisted connector records.
        static const Connector * resolveConnector(const std::string & connector_name, ConnectorGraphContext & graph_context);

        // Validate all referenced transformations are present.
        static bool validateConnectorTransformations(const Connector & connector, const IRegistryStore & store);

        // Validate optional condition reference is present.
        static bool validateConnectorCondition(const Connector & connector, const IRegistryStore & store);

        // Iteratively compute open-slot count with cycle detection and memoization.
        static std::optional<std::uint32_t> computeOpenSlotsIterative(
            const std::string & root_connector_name,
            ConnectorGraphContext & context);

        // Iteratively resolve produced scalar entries with cycle detection and memoization.
        static std::optional<std::shared_ptr<const dcn::chain::ResolvedScalarEntries>> computeScalarEntriesIterative(
            const std::string & root_connector_name,
            ConnectorGraphContext & context);

        // Append local scalar entry for connector scalar dimension.
        static void appendLocalScalarEntry(
            const std::string & connector_name,
            std::uint32_t dim_id,
            dcn::chain::ResolvedScalarEntries & connector_scalar_entries);

        static const Connector * resolveConnector(const std::string & connector_name, ConnectorGraphContext & graph_context)
        {
            if(connector_name == graph_context.pending_connector.name())
            {
                return &graph_context.pending_connector;
            }

            if(graph_context.staged_connectors != nullptr)
            {
                const auto staged_it = graph_context.staged_connectors->find(connector_name);
                if(staged_it != graph_context.staged_connectors->end())
                {
                    return &staged_it->second;
                }
            }

            const auto cache_it = graph_context.connector_cache.find(connector_name);
            if(cache_it != graph_context.connector_cache.end())
            {
                return &cache_it->second;
            }

            const auto record_handle_opt = graph_context.store.getConnectorRecordHandle(connector_name);
            if(!record_handle_opt.has_value() || !(*record_handle_opt))
            {
                return nullptr;
            }

            auto [it, _] = graph_context.connector_cache.try_emplace(connector_name, (*record_handle_opt)->connector());
            return &it->second;
        }

        static bool validateConnectorTransformations(const Connector & connector, const IRegistryStore & store)
        {
            for(const Dimension & dimension : connector.dimensions())
            {
                for(const auto & transformation : dimension.transformations())
                {
                    if(transformation.name().empty())
                    {
                        spdlog::error("Connector `{}` contains unnamed transformation", connector.name());
                        return false;
                    }

                    if(!store.hasTransformation(transformation.name()))
                    {
                        spdlog::error(
                            "Cannot find transformation `{}` used in connector `{}`",
                            transformation.name(),
                            connector.name());
                        return false;
                    }
                }
            }

            return true;
        }

        static bool validateConnectorCondition(const Connector & connector, const IRegistryStore & store)
        {
            if(connector.condition_name().empty())
            {
                return true;
            }

            if(store.hasCondition(connector.condition_name()))
            {
                return true;
            }

            spdlog::error(
                "Cannot find condition `{}` used in connector `{}`",
                connector.condition_name(),
                connector.name());
            return false;
        }

        static std::optional<std::uint32_t> computeOpenSlotsIterative(
            const std::string & root_connector_name,
            ConnectorGraphContext & context)
        {
            struct StackFrame
            {
                std::string connector_name;
                bool post_visit = false;
            };

            absl::flat_hash_map<std::string, VisitState> visit_state;
            absl::flat_hash_map<std::string, std::uint32_t> open_slots_cache;
            std::vector<StackFrame> stack;
            stack.push_back(StackFrame{.connector_name = root_connector_name, .post_visit = false});

            while(!stack.empty())
            {
                StackFrame frame = std::move(stack.back());
                stack.pop_back();

                if(frame.post_visit)
                {
                    const Connector * connector = resolveConnector(frame.connector_name, context);
                    if(connector == nullptr)
                    {
                        spdlog::error(
                            "Cannot resolve connector definition `{}` while validating `{}`",
                            frame.connector_name,
                            root_connector_name);
                        return std::nullopt;
                    }

                    std::uint64_t open_slots = 0;
                    for(std::uint32_t dim_id = 0; dim_id < static_cast<std::uint32_t>(connector->dimensions_size()); ++dim_id)
                    {
                        const Dimension & dimension = connector->dimensions(static_cast<int>(dim_id));
                        const std::string & composite_name = dimension.composite();

                        if(composite_name.empty())
                        {
                            if(dimension.bindings_size() != 0)
                            {
                                spdlog::error(
                                    "Connector `{}` has bindings on scalar dimension {}",
                                    frame.connector_name,
                                    dim_id);
                                return std::nullopt;
                            }

                            open_slots += 1;
                            continue;
                        }

                        const auto child_it = open_slots_cache.find(composite_name);
                        if(child_it == open_slots_cache.end())
                        {
                            spdlog::error(
                                "Failed to resolve open slots for composite connector `{}` in `{}`",
                                composite_name,
                                frame.connector_name);
                            return std::nullopt;
                        }

                        const std::uint32_t child_open_slots = child_it->second;
                        open_slots += child_open_slots;

                        absl::flat_hash_set<std::uint32_t> bound_slot_ids;
                        std::uint64_t bound_open_slots = 0;
                        for(const auto & [slot_str, binding_target] : dimension.bindings())
                        {
                            if(binding_target.empty())
                            {
                                spdlog::error(
                                    "Connector `{}` has empty binding target at dim {} slot `{}`",
                                    frame.connector_name,
                                    dim_id,
                                    slot_str);
                                return std::nullopt;
                            }

                            const auto slot_id_opt = parse::parseUint32Decimal(slot_str);
                            if(!slot_id_opt)
                            {
                                spdlog::error(
                                    "Connector `{}` has invalid binding slot id `{}` at dim {}",
                                    frame.connector_name,
                                    slot_str,
                                    dim_id);
                                return std::nullopt;
                            }

                            const std::uint32_t slot_id = *slot_id_opt;
                            if(!bound_slot_ids.insert(slot_id).second)
                            {
                                spdlog::error(
                                    "Connector `{}` has duplicate binding slot id {} at dim {}",
                                    frame.connector_name,
                                    slot_id,
                                    dim_id);
                                return std::nullopt;
                            }

                            if(slot_id >= child_open_slots)
                            {
                                spdlog::error(
                                    "Connector `{}` binding slot {} is out of range for child `{}` (open slots: {})",
                                    frame.connector_name,
                                    slot_id,
                                    composite_name,
                                    child_open_slots);
                                return std::nullopt;
                            }

                            const auto target_it = open_slots_cache.find(binding_target);
                            if(target_it == open_slots_cache.end())
                            {
                                spdlog::error(
                                    "Cannot resolve binding target `{}` in connector `{}` dim {} slot {}",
                                    binding_target,
                                    frame.connector_name,
                                    dim_id,
                                    slot_id);
                                return std::nullopt;
                            }

                            bound_open_slots += static_cast<std::uint64_t>(target_it->second);
                        }

                        open_slots += bound_open_slots;
                        open_slots -= static_cast<std::uint64_t>(bound_slot_ids.size());
                    }

                    if(open_slots > std::numeric_limits<std::uint32_t>::max())
                    {
                        spdlog::error("Connector `{}` exceeds supported open slots range", frame.connector_name);
                        return std::nullopt;
                    }

                    visit_state[frame.connector_name] = VisitState::DONE;
                    open_slots_cache[frame.connector_name] = static_cast<std::uint32_t>(open_slots);
                    continue;
                }

                if(open_slots_cache.contains(frame.connector_name))
                {
                    continue;
                }

                const auto state_it = visit_state.find(frame.connector_name);
                if(state_it != visit_state.end())
                {
                    if(state_it->second == VisitState::VISITING)
                    {
                        spdlog::error("Connector dependency cycle detected at `{}`", frame.connector_name);
                        return std::nullopt;
                    }

                    if(state_it->second == VisitState::DONE)
                    {
                        continue;
                    }
                }

                const Connector * connector = resolveConnector(frame.connector_name, context);
                if(connector == nullptr)
                {
                    spdlog::error(
                        "Cannot resolve connector definition `{}` while validating `{}`",
                        frame.connector_name,
                        root_connector_name);
                    return std::nullopt;
                }

                visit_state[frame.connector_name] = VisitState::VISITING;
                stack.push_back(StackFrame{.connector_name = frame.connector_name, .post_visit = true});

                for(const Dimension & dimension : connector->dimensions())
                {
                    const std::string & composite_name = dimension.composite();
                    if(composite_name.empty())
                    {
                        continue;
                    }

                    stack.push_back(StackFrame{.connector_name = composite_name, .post_visit = false});

                    for(const auto & [slot_str, binding_target] : dimension.bindings())
                    {
                        if(binding_target.empty())
                        {
                            spdlog::error(
                                "Connector `{}` has empty binding target at slot `{}`",
                                frame.connector_name,
                                slot_str);
                            return std::nullopt;
                        }

                        const auto slot_id_opt = parse::parseUint32Decimal(slot_str);
                        if(!slot_id_opt)
                        {
                            spdlog::error(
                                "Connector `{}` has invalid binding slot id `{}`",
                                frame.connector_name,
                                slot_str);
                            return std::nullopt;
                        }

                        stack.push_back(StackFrame{.connector_name = binding_target, .post_visit = false});
                    }
                }
            }

            const auto root_it = open_slots_cache.find(root_connector_name);
            if(root_it == open_slots_cache.end())
            {
                return std::nullopt;
            }

            return root_it->second;
        }

        static void appendLocalScalarEntry(
            const std::string & connector_name,
            std::uint32_t dim_id,
            dcn::chain::ResolvedScalarEntries & connector_scalar_entries)
        {
            const evmc::bytes32 dim_path_hash = dcn::chain::dimPathHash(dim_id);
            const evmc::bytes32 scalar_hash = dcn::chain::keccakString(connector_name);
            connector_scalar_entries.hash_entries.push_back(dcn::chain::ScalarHashEntry{
                .scalar_hash = scalar_hash,
                .path_hash = dim_path_hash
            });
            connector_scalar_entries.display_entries.push_back(ScalarLabel{
                .scalar = connector_name,
                .path_hash = dim_path_hash,
                .tail_id = dim_id
            });
        }

        static std::optional<std::shared_ptr<const dcn::chain::ResolvedScalarEntries>> computeScalarEntriesIterative(
            const std::string & root_connector_name,
            ConnectorGraphContext & context)
        {
            struct StackFrame
            {
                std::string connector_name;
                bool post_visit = false;
            };

            using ScalarEntriesPtr = std::shared_ptr<const dcn::chain::ResolvedScalarEntries>;

            absl::flat_hash_map<std::string, VisitState> visit_state;
            absl::flat_hash_map<std::string, ScalarEntriesPtr> scalar_entries_cache;
            std::vector<StackFrame> stack;
            stack.push_back(StackFrame{.connector_name = root_connector_name, .post_visit = false});

            while(!stack.empty())
            {
                StackFrame frame = std::move(stack.back());
                stack.pop_back();

                if(frame.post_visit)
                {
                    const Connector * connector = resolveConnector(frame.connector_name, context);
                    if(connector == nullptr)
                    {
                        spdlog::error(
                            "Cannot resolve connector definition `{}` while resolving scalars for `{}`",
                            frame.connector_name,
                            root_connector_name);
                        return std::nullopt;
                    }

                    auto connector_scalar_entries = std::make_shared<dcn::chain::ResolvedScalarEntries>();

                    for(std::uint32_t dim_id = 0; dim_id < static_cast<std::uint32_t>(connector->dimensions_size()); ++dim_id)
                    {
                        const Dimension & dimension = connector->dimensions(static_cast<int>(dim_id));
                        const std::string & composite_name = dimension.composite();
                        if(composite_name.empty())
                        {
                            if(dimension.bindings_size() != 0)
                            {
                                spdlog::error(
                                    "Connector `{}` has bindings on scalar dimension {} while resolving scalar slots",
                                    frame.connector_name,
                                    dim_id);
                                return std::nullopt;
                            }

                            appendLocalScalarEntry(frame.connector_name, dim_id, *connector_scalar_entries);
                            continue;
                        }

                        const auto child_it = scalar_entries_cache.find(composite_name);
                        if(child_it == scalar_entries_cache.end() || !child_it->second)
                        {
                            spdlog::error(
                                "Cannot resolve scalar entries for child `{}` in connector `{}`",
                                composite_name,
                                frame.connector_name);
                            return std::nullopt;
                        }

                        const dcn::chain::ResolvedScalarEntries & child_scalar_entries = *child_it->second;
                        if(child_scalar_entries.hash_entries.size() != child_scalar_entries.display_entries.size())
                        {
                            spdlog::error(
                                "Connector `{}` child `{}` has inconsistent scalar-entry cache",
                                frame.connector_name,
                                composite_name);
                            return std::nullopt;
                        }

                        if(
                            child_scalar_entries.display_entries.size() >
                            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
                        {
                            spdlog::error(
                                "Connector `{}` child `{}` scalar labels exceed uint32 range",
                                frame.connector_name,
                                composite_name);
                            return std::nullopt;
                        }

                        absl::flat_hash_map<std::uint32_t, std::string> binding_by_slot;
                        for(const auto & [slot_str, binding_target] : dimension.bindings())
                        {
                            if(binding_target.empty())
                            {
                                spdlog::error(
                                    "Connector `{}` has empty binding target at dim {} slot `{}` while resolving scalar slots",
                                    frame.connector_name,
                                    dim_id,
                                    slot_str);
                                return std::nullopt;
                            }

                            const auto slot_id_opt = parse::parseUint32Decimal(slot_str);
                            if(!slot_id_opt)
                            {
                                spdlog::error(
                                    "Connector `{}` has invalid binding slot id `{}` at dim {} while resolving scalar slots",
                                    frame.connector_name,
                                    slot_str,
                                    dim_id);
                                return std::nullopt;
                            }

                            const std::uint32_t slot_id = *slot_id_opt;
                            if(slot_id >= child_scalar_entries.display_entries.size())
                            {
                                spdlog::error(
                                    "Connector `{}` binding slot {} is out of range for child `{}` while resolving scalar labels",
                                    frame.connector_name,
                                    slot_id,
                                    composite_name);
                                return std::nullopt;
                            }

                            if(!binding_by_slot.try_emplace(slot_id, binding_target).second)
                            {
                                spdlog::error(
                                    "Connector `{}` has duplicate binding slot id {} at dim {} while resolving scalar slots",
                                    frame.connector_name,
                                    slot_id,
                                    dim_id);
                                return std::nullopt;
                            }
                        }

                        for(std::size_t slot_index = 0; slot_index < child_scalar_entries.display_entries.size(); ++slot_index)
                        {
                            const std::uint32_t slot_id = static_cast<std::uint32_t>(slot_index);
                            const auto binding_it = binding_by_slot.find(slot_id);
                            if(binding_it == binding_by_slot.end())
                            {
                                connector_scalar_entries->hash_entries.push_back(child_scalar_entries.hash_entries[slot_index]);
                                connector_scalar_entries->display_entries.push_back(child_scalar_entries.display_entries[slot_index]);
                                continue;
                            }

                            const auto resolved_binding_it = scalar_entries_cache.find(binding_it->second);
                            if(resolved_binding_it == scalar_entries_cache.end() || !resolved_binding_it->second)
                            {
                                spdlog::error(
                                    "Cannot resolve binding scalar entries for `{}` in connector `{}`",
                                    binding_it->second,
                                    frame.connector_name);
                                return std::nullopt;
                            }

                            const dcn::chain::ResolvedScalarEntries & binding_scalar_entries = *resolved_binding_it->second;
                            if(binding_scalar_entries.hash_entries.size() != binding_scalar_entries.display_entries.size())
                            {
                                spdlog::error(
                                    "Connector `{}` binding `{}` has inconsistent scalar-entry cache",
                                    frame.connector_name,
                                    binding_it->second);
                                return std::nullopt;
                            }

                            for(std::size_t i = 0; i < binding_scalar_entries.display_entries.size(); ++i)
                            {
                                connector_scalar_entries->hash_entries.push_back(binding_scalar_entries.hash_entries[i]);
                                connector_scalar_entries->display_entries.push_back(binding_scalar_entries.display_entries[i]);
                            }
                        }
                    }

                    visit_state[frame.connector_name] = VisitState::DONE;
                    scalar_entries_cache[frame.connector_name] = connector_scalar_entries;
                    continue;
                }

                if(scalar_entries_cache.contains(frame.connector_name))
                {
                    continue;
                }

                const auto state_it = visit_state.find(frame.connector_name);
                if(state_it != visit_state.end())
                {
                    if(state_it->second == VisitState::VISITING)
                    {
                        spdlog::error(
                            "Connector dependency cycle detected while resolving scalars at `{}`",
                            frame.connector_name);
                        return std::nullopt;
                    }

                    if(state_it->second == VisitState::DONE)
                    {
                        continue;
                    }
                }

                const Connector * connector = resolveConnector(frame.connector_name, context);
                if(connector == nullptr)
                {
                    spdlog::error(
                        "Cannot resolve connector definition `{}` while resolving scalars for `{}`",
                        frame.connector_name,
                        root_connector_name);
                    return std::nullopt;
                }

                visit_state[frame.connector_name] = VisitState::VISITING;
                stack.push_back(StackFrame{.connector_name = frame.connector_name, .post_visit = true});

                for(const Dimension & dimension : connector->dimensions())
                {
                    const std::string & composite_name = dimension.composite();
                    if(composite_name.empty())
                    {
                        continue;
                    }

                    stack.push_back(StackFrame{.connector_name = composite_name, .post_visit = false});

                    for(const auto & [slot_str, binding_target] : dimension.bindings())
                    {
                        if(binding_target.empty())
                        {
                            spdlog::error(
                                "Connector `{}` has empty binding target at slot `{}` while traversing scalar dependencies",
                                frame.connector_name,
                                slot_str);
                            return std::nullopt;
                        }

                        const auto slot_id_opt = parse::parseUint32Decimal(slot_str);
                        if(!slot_id_opt)
                        {
                            spdlog::error(
                                "Connector `{}` has invalid binding slot id `{}` while traversing scalar dependencies",
                                frame.connector_name,
                                slot_str);
                            return std::nullopt;
                        }

                        stack.push_back(StackFrame{.connector_name = binding_target, .post_visit = false});
                    }
                }
            }

            const auto root_it = scalar_entries_cache.find(root_connector_name);
            if(root_it == scalar_entries_cache.end() || !root_it->second)
            {
                return std::nullopt;
            }

            return root_it->second;
        }

        // Validate slots/bindings and resolve the connector's composite-aware scalar
        // entries. Single source of truth for the format-hash inputs, shared by
        // addConnector, addConnectorsBatch, and computeConnectorFormatHash.
        static std::optional<std::shared_ptr<const dcn::chain::ResolvedScalarEntries>> resolveConnectorScalarEntries(
            const std::string & connector_name,
            ConnectorGraphContext & graph_context)
        {
            if(!computeOpenSlotsIterative(connector_name, graph_context).has_value())
            {
                spdlog::error("Connector `{}` failed slot/binding validation", connector_name);
                return std::nullopt;
            }

            const auto root_scalar_entries_opt = computeScalarEntriesIterative(connector_name, graph_context);
            if(
                !root_scalar_entries_opt.has_value() ||
                !(*root_scalar_entries_opt) ||
                (*root_scalar_entries_opt)->hash_entries.empty() ||
                (*root_scalar_entries_opt)->hash_entries.size() != (*root_scalar_entries_opt)->display_entries.size())
            {
                spdlog::error(
                    "Connector `{}` produced invalid scalar entries while computing format hash",
                    connector_name);
                return std::nullopt;
            }

            return *root_scalar_entries_opt;
        }

        template<typename RecordT>
        static bool recordsEqual(const RecordT & lhs, const RecordT & rhs)
        {
            if constexpr(std::is_same_v<RecordT, ConnectorRecord>)
            {
                ConnectorRecord lhs_normalized = lhs;
                ConnectorRecord rhs_normalized = rhs;

                lhs_normalized.mutable_connector()->clear_static_ri();
                rhs_normalized.mutable_connector()->clear_static_ri();

                std::string lhs_bytes;
                std::string rhs_bytes;
                if(!lhs_normalized.SerializeToString(&lhs_bytes) || !rhs_normalized.SerializeToString(&rhs_bytes))
                {
                    return false;
                }

                return lhs_bytes == rhs_bytes;
            }

            std::string lhs_bytes;
            std::string rhs_bytes;
            if(!lhs.SerializeToString(&lhs_bytes) || !rhs.SerializeToString(&rhs_bytes))
            {
                return false;
            }

            return lhs_bytes == rhs_bytes;
        }

        // The registry mirrors only chain-derivable state. Executable source remains in the
        // content-addressed artifact, while args_count is stored for reads and validation.
        static void normalizeForRegistry(TransformationRecord & record)
        {
            auto * transformation = record.mutable_transformation();
            if(transformation->sol_src().empty())
            {
                return;
            }

            const auto args_count = countTransformationArgs(transformation->sol_src());
            if(args_count)
            {
                transformation->set_args_count(*args_count);
            }
            else
            {
                spdlog::warn(
                    "Failed to compute args_count for transformation `{}`: {}",
                    transformation->name(),
                    args_count.error().message);
            }
            transformation->clear_sol_src();
        }

        static void normalizeForRegistry(ConditionRecord & record)
        {
            auto * condition = record.mutable_condition();
            if(condition->sol_src().empty())
            {
                return;
            }

            const auto args_count = countConditionArgs(condition->sol_src());
            if(args_count)
            {
                condition->set_args_count(*args_count);
            }
            else
            {
                spdlog::warn(
                    "Failed to compute args_count for condition `{}`: {}",
                    condition->name(),
                    args_count.error().message);
            }
            condition->clear_sol_src();
        }

        template<typename KeyT>
        static std::string cacheKeyToString(const KeyT & key)
        {
            if constexpr(std::is_same_v<KeyT, std::string>)
            {
                return key;
            }
            else if constexpr(std::is_same_v<KeyT, chain::Address>)
            {
                return evmc::hex(key);
            }
            else
            {
                return "<unknown-key-type>";
            }
        }

        template<typename KeyT, typename ValueT, typename ValueArgT>
        static void putHotCacheEntry(
            LruCache<KeyT, ValueT> & cache,
            const KeyT & key,
            ValueArgT && value)
        {
            if(cache.capacity == 0)
            {
                return;
            }

            const auto it = cache.entries.find(key);
            if(it != cache.entries.end())
            {
                it->second.value = std::forward<ValueArgT>(value);
                cache.order.splice(cache.order.begin(), cache.order, it->second.order_it);
                it->second.order_it = cache.order.begin();
                spdlog::debug("Cache update [{}] key={}", cache.name, cacheKeyToString(key));
                return;
            }

            if(cache.entries.size() >= cache.capacity)
            {
                const KeyT & lru_key = cache.order.back();
                spdlog::debug("Cache remove [{}] key={} (evicted LRU)", cache.name, cacheKeyToString(lru_key));
                cache.entries.erase(lru_key);
                cache.order.pop_back();
            }

            cache.order.push_front(key);
            const auto order_it = cache.order.begin();
            cache.entries.insert_or_assign(
                *order_it,
                typename LruCache<KeyT, ValueT>::Entry{
                    .value = std::forward<ValueArgT>(value),
                    .order_it = order_it});
            spdlog::debug("Cache add [{}] key={}", cache.name, cacheKeyToString(key));
        }

        template<typename KeyT, typename ValueT>
        static const ValueT * getHotCacheEntry(LruCache<KeyT, ValueT> & cache, const KeyT & key)
        {
            const auto it = cache.entries.find(key);
            if(it == cache.entries.end())
            {
                return nullptr;
            }

            cache.order.splice(cache.order.begin(), cache.order, it->second.order_it);
            it->second.order_it = cache.order.begin();
            return &it->second.value;
        }

        template<typename KeyT, typename ValueT>
        static void clearHotCache(LruCache<KeyT, ValueT> & cache, const char * reason)
        {
            if(cache.entries.empty())
            {
                return;
            }

            for(const auto & [key, entry] : cache.entries)
            {
                (void)entry;
                spdlog::debug("Cache remove [{}] key={} ({})", cache.name, cacheKeyToString(key), reason);
            }

            cache.entries.clear();
            cache.order.clear();
        }
    }


    Registry::Registry(asio::io_context & io_context, std::string sqlite_path)
        : _strand(asio::make_strand(io_context))
        , _store(std::make_unique<SQLiteRegistryStore>(std::move(sqlite_path)))
    {
        _connector_record_cache.name = "connector-record";
        _format_hash_cache.name = "format-hash";
        _transformation_record_cache.name = "transformation-record";
        _condition_record_cache.name = "condition-record";

        _connector_record_cache.capacity = kHotCacheCapacity;
        _format_hash_cache.capacity = kHotCacheCapacity;
        _transformation_record_cache.capacity = kHotCacheCapacity;
        _condition_record_cache.capacity = kHotCacheCapacity;
    }

    asio::awaitable<bool> Registry::addConnector(chain::Address address, ConnectorRecord record,
        std::optional<evmc::bytes32> expected_format_hash)
    {
        co_return co_await addConnectorVersion(
            EntitySource{}, chain::Address{}, std::move(record), expected_format_hash);
    }

    asio::awaitable<bool> Registry::addConnectorVersion(
        EntitySource source,
        chain::Address address,
        ConnectorRecord record,
        std::optional<evmc::bytes32> expected_format_hash)
    {
        const Connector & connector = record.connector();
        const std::string connector_name = connector.name();
        if(connector_name.empty())
        {
            spdlog::error("Connector name is empty");
            co_return false;
        }

        co_await async::ensureOnStrand(_strand);

        if(connector.dimensions_size() <= 0)
        {
            spdlog::error("Connector `{}` has zero dimensions", connector_name);
            co_return false;
        }

        if(!validateConnectorTransformations(connector, *_store))
        {
            co_return false;
        }

        if(!validateConnectorCondition(connector, *_store))
        {
            co_return false;
        }

        ConnectorGraphContext graph_context{
            .pending_connector = connector,
            .store = *_store,
            .connector_cache = {},
        };

        const auto root_scalar_entries_opt = resolveConnectorScalarEntries(connector_name, graph_context);
        if(!root_scalar_entries_opt)
        {
            co_return false;
        }

        const evmc::bytes32 format_hash = chain::computeFormatHash((*root_scalar_entries_opt)->hash_entries);

        if(expected_format_hash.has_value() && !chain::equalBytes32(*expected_format_hash, format_hash))
        {
            spdlog::error(
                "Connector `{}` chain-emitted format hash {} diverges from locally computed {} — refusing to materialize",
                connector_name,
                evmc::hex(*expected_format_hash),
                evmc::hex(format_hash));
            co_return false;
        }

        const std::vector<ScalarLabel> canonical_scalar_labels =
            chain::canonicalizeScalarLabels((*root_scalar_entries_opt)->display_entries);

        const auto owner_opt = evmc::from_hex<chain::Address>(record.owner());
        if(!owner_opt)
        {
            spdlog::error("Failed to parse owner address from hex string '{}'", record.owner());
            co_return false;
        }

        const auto existing_scalar_labels = _store->getScalarLabelsByFormatHash(format_hash);
        if(existing_scalar_labels.has_value() && !chain::scalarLabelsEqual(*existing_scalar_labels, canonical_scalar_labels))
        {
            spdlog::warn(
                "Different scalar-label sets for format hash {} while registering connector '{}' at runtime address {} "
                "(stored_labels={}, new_labels={}); "
                "keeping first inserted representation",
                evmc::hex(format_hash),
                connector_name,
                evmc::hex(address),
                existing_scalar_labels->size(),
                canonical_scalar_labels.size());
        }

        const auto existing_record_handle_opt = _store->getConnectorRecordHandle(connector_name);
        if(existing_record_handle_opt.has_value() && *existing_record_handle_opt && source.source_id == "local")
        {
            if(!recordsEqual(*(*existing_record_handle_opt), record))
            {
                spdlog::error(
                    "Connector `{}` already exists with different payload",
                    connector_name);
                co_return false;
            }

            const auto existing_format_hash = _store->getConnectorFormatHash(connector_name);
            if(!existing_format_hash.has_value() || !chain::equalBytes32(*existing_format_hash, format_hash))
            {
                spdlog::error(
                    "Connector `{}` already exists with different format hash",
                    connector_name);
                co_return false;
            }

            putHotCacheEntry(
                _connector_record_cache,
                connector_name,
                existing_record_handle_opt);
            putHotCacheEntry(
                _format_hash_cache,
                connector_name,
                std::optional<evmc::bytes32>(format_hash));
            co_return true;
        }

        if(!_store->addConnectorVersion(source, address, record, format_hash, canonical_scalar_labels))
        {
            co_return false;
        }

        putHotCacheEntry(
            _connector_record_cache,
            connector_name,
            std::optional<ConnectorRecordHandle>(std::make_shared<ConnectorRecord>(record)));
        putHotCacheEntry(
            _format_hash_cache,
            connector_name,
            std::optional<evmc::bytes32>(format_hash));

        co_return true;
    }

    asio::awaitable<bool> Registry::addConnectorsBatch(
        std::vector<std::pair<chain::Address, ConnectorRecord>> connectors,
        bool all_or_nothing)
    {
        co_await async::ensureOnStrand(_strand);

        std::vector<ConnectorBatchItem> batch_items;
        batch_items.reserve(connectors.size());
        absl::flat_hash_map<std::string, Connector> staged_connectors;
        staged_connectors.reserve(connectors.size());
        absl::flat_hash_set<std::string> seen_names;
        seen_names.reserve(connectors.size());
        bool all_valid = true;

        for(auto & [address, record] : connectors)
        {
            const Connector & connector = record.connector();
            const std::string connector_name = connector.name();
            if(connector_name.empty())
            {
                spdlog::error("Connector name is empty");
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            if(!seen_names.insert(connector_name).second)
            {
                spdlog::error("Duplicate connector name in batch for `{}`", connector_name);
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            if(connector.dimensions_size() <= 0)
            {
                spdlog::error("Connector `{}` has zero dimensions", connector_name);
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            if(!validateConnectorTransformations(connector, *_store))
            {
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            if(!validateConnectorCondition(connector, *_store))
            {
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            ConnectorGraphContext graph_context{
                .pending_connector = connector,
                .store = *_store,
                .staged_connectors = &staged_connectors,
                .connector_cache = {},
            };

            const auto root_scalar_entries_opt = resolveConnectorScalarEntries(connector_name, graph_context);
            if(!root_scalar_entries_opt)
            {
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            const evmc::bytes32 format_hash = chain::computeFormatHash((*root_scalar_entries_opt)->hash_entries);
            std::vector<ScalarLabel> canonical_scalar_labels =
                chain::canonicalizeScalarLabels((*root_scalar_entries_opt)->display_entries);

            const auto owner_opt = evmc::from_hex<chain::Address>(record.owner());
            if(!owner_opt)
            {
                spdlog::error("Failed to parse owner address from hex string '{}'", record.owner());
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            const auto existing_scalar_labels = _store->getScalarLabelsByFormatHash(format_hash);
            if(existing_scalar_labels.has_value() && !chain::scalarLabelsEqual(*existing_scalar_labels, canonical_scalar_labels))
            {
                spdlog::warn(
                    "Different scalar-label sets for format hash {} while registering connector '{}' at runtime address {} "
                    "(stored_labels={}, new_labels={}); "
                    "keeping first inserted representation",
                    evmc::hex(format_hash),
                    connector_name,
                    evmc::hex(address),
                    existing_scalar_labels->size(),
                    canonical_scalar_labels.size());
            }

            const auto existing_record_handle_opt = _store->getConnectorRecordHandle(connector_name);
            if(existing_record_handle_opt.has_value() && *existing_record_handle_opt)
            {
                if(!recordsEqual(*(*existing_record_handle_opt), record))
                {
                    spdlog::error(
                        "Connector `{}` already exists with different payload",
                        connector_name);
                    all_valid = false;
                    if(all_or_nothing)
                    {
                        co_return false;
                    }
                    continue;
                }

                const auto existing_format_hash = _store->getConnectorFormatHash(connector_name);
                if(!existing_format_hash.has_value() || !chain::equalBytes32(*existing_format_hash, format_hash))
                {
                    spdlog::error(
                        "Connector `{}` already exists with different format hash",
                        connector_name);
                    all_valid = false;
                    if(all_or_nothing)
                    {
                        co_return false;
                    }
                    continue;
                }

                putHotCacheEntry(
                    _connector_record_cache,
                    connector_name,
                    existing_record_handle_opt);
                putHotCacheEntry(
                    _format_hash_cache,
                    connector_name,
                    std::optional<evmc::bytes32>(format_hash));
                staged_connectors.insert_or_assign(connector_name, connector);
                continue;
            }

            staged_connectors.insert_or_assign(connector_name, connector);
            batch_items.push_back(ConnectorBatchItem{
                .address = address,
                .record = std::move(record),
                .format_hash = format_hash,
                .canonical_scalar_labels = std::move(canonical_scalar_labels)
            });
        }

        if(batch_items.empty())
        {
            co_return all_valid;
        }

        const bool inserted = _store->addConnectorsBatch(batch_items, all_or_nothing);
        if(!inserted && all_or_nothing)
        {
            co_return false;
        }

        for(const ConnectorBatchItem & item : batch_items)
        {
            const std::string & connector_name = item.record.connector().name();
            if(!_store->hasConnector(connector_name))
            {
                continue;
            }

            putHotCacheEntry(
                _connector_record_cache,
                connector_name,
                std::optional<ConnectorRecordHandle>(std::make_shared<ConnectorRecord>(item.record)));
            putHotCacheEntry(
                _format_hash_cache,
                connector_name,
                std::optional<evmc::bytes32>(item.format_hash));
        }

        co_return inserted && all_valid;
    }

    asio::awaitable<std::optional<ConnectorRecordHandle>> Registry::getConnectorRecordHandle(
        const std::string & name) const
    {
        spdlog::debug("Registry::getConnectorRecordHandle('{}'): enter", name);

        co_await async::ensureOnStrand(_strand);
        spdlog::debug("Registry::getConnectorRecordHandle('{}'): on strand", name);

        if(const auto * cached = getHotCacheEntry(_connector_record_cache, name))
        {
            spdlog::debug("Registry::getConnectorRecordHandle('{}'): cache hit has_value={}", name, cached->has_value());
            co_return *cached;
        }

        const auto record_handle_opt = _store->getConnectorRecordHandle(name);

        spdlog::debug(
            "Registry::getConnectorRecordHandle('{}'): store result has_value={}, has_record={}",
            name,
            record_handle_opt.has_value(),
            record_handle_opt.has_value() ? static_cast<bool>(*record_handle_opt) : false);

        putHotCacheEntry(_connector_record_cache, name, record_handle_opt);
        
        spdlog::debug("Registry::getConnectorRecordHandle('{}'): done", name);

        co_return record_handle_opt;
    }

    asio::awaitable<bool> Registry::hasConnector(const std::string & name) const
    {
        spdlog::debug("Registry::hasConnector('{}'): enter", name);

        co_await async::ensureOnStrand(_strand);
        
        spdlog::debug("Registry::hasConnector('{}'): on strand", name);

        if(const auto * cached = getHotCacheEntry(_connector_record_cache, name))
        {
            const bool cache_has_record = cached->has_value() && static_cast<bool>(cached->value());
            spdlog::debug("Registry::hasConnector('{}'): cache hit has_record={}", name, cache_has_record);
            if(cache_has_record)
            {
                co_return true;
            }
        }

        const bool exists = _store->hasConnector(name);
        
        spdlog::debug("Registry::hasConnector('{}'): store result={}", name, exists);
        
        if(!exists)
        {
            putHotCacheEntry(_connector_record_cache, name, std::nullopt);
        }
        co_return exists;
    }

    asio::awaitable<std::optional<evmc::bytes32>> Registry::getFormatHash(const std::string & name) const
    {
        co_await async::ensureOnStrand(_strand);

        if(const auto * cached = getHotCacheEntry(_format_hash_cache, name))
        {
            co_return *cached;
        }

        const auto format_hash_opt = _store->getConnectorFormatHash(name);
        putHotCacheEntry(_format_hash_cache, name, format_hash_opt);
        co_return format_hash_opt;
    }

    asio::awaitable<std::optional<evmc::bytes32>> Registry::computeConnectorFormatHash(Connector connector) const
    {
        co_await async::ensureOnStrand(_strand);

        ConnectorGraphContext graph_context{
            .pending_connector = connector,
            .store = *_store,
            .connector_cache = {},
        };

        const auto root_scalar_entries_opt = resolveConnectorScalarEntries(connector.name(), graph_context);
        if(!root_scalar_entries_opt)
        {
            co_return std::nullopt;
        }

        co_return chain::computeFormatHash((*root_scalar_entries_opt)->hash_entries);
    }

    asio::awaitable<std::size_t> Registry::getFormatConnectorNamesCount(const evmc::bytes32 & format_hash) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getFormatConnectorNamesCount(format_hash);
    }

    asio::awaitable<NameCursorPage> Registry::getFormatConnectorNamesCursor(
        const evmc::bytes32 & format_hash,
        const std::optional<NameCursor> & after,
        std::size_t limit) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getFormatConnectorNamesCursor(format_hash, after, limit);
    }

    asio::awaitable<std::size_t> Registry::getFormatsCount() const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getFormatsCount();
    }

    asio::awaitable<NameCursorPage> Registry::getFormatsCursor(
        const std::optional<evmc::bytes32> & after,
        std::size_t limit) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getFormatsCursor(after, limit);
    }

    asio::awaitable<std::optional<std::vector<ScalarLabel>>> Registry::getScalarLabelsByFormatHash(
        const evmc::bytes32 & format_hash) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getScalarLabelsByFormatHash(format_hash);
    }

    asio::awaitable<bool> Registry::addTransformation(chain::Address address, TransformationRecord record)
    {
        co_return co_await addTransformationVersion(EntitySource{}, chain::Address{}, std::move(record));
    }

    asio::awaitable<bool> Registry::addTransformationVersion(
        EntitySource source,
        chain::Address address,
        TransformationRecord record)
    {
        const std::string transformation_name = record.transformation().name();
        if(transformation_name.empty())
        {
            spdlog::error("Transformation name is empty");
            co_return false;
        }

        co_await async::ensureOnStrand(_strand);

        normalizeForRegistry(record);

        const auto existing_record_handle_opt = _store->getTransformationRecordHandle(transformation_name);
        if(existing_record_handle_opt.has_value() && *existing_record_handle_opt && source.source_id == "local")
        {
            if(!recordsEqual(*(*existing_record_handle_opt), record))
            {
                spdlog::error(
                    "Transformation `{}` already exists with different payload",
                    transformation_name);
                co_return false;
            }

            putHotCacheEntry(
                _transformation_record_cache,
                transformation_name,
                existing_record_handle_opt);
            co_return true;
        }

        const bool inserted = _store->addTransformationVersion(source, address, record);
        if(inserted)
        {
            putHotCacheEntry(
                _transformation_record_cache,
                transformation_name,
                std::optional<TransformationRecordHandle>(
                    std::make_shared<TransformationRecord>(record)));
        }

        co_return inserted;
    }

    asio::awaitable<bool> Registry::addTransformationsBatch(
        std::vector<std::pair<chain::Address, TransformationRecord>> transformations,
        bool all_or_nothing)
    {
        co_await async::ensureOnStrand(_strand);

        std::vector<TransformationBatchItem> batch_items;
        batch_items.reserve(transformations.size());
        absl::flat_hash_set<std::string> seen_names;
        seen_names.reserve(transformations.size());
        bool all_valid = true;
        for(auto & [address, record] : transformations)
        {
            const std::string transformation_name = record.transformation().name();
            if(transformation_name.empty())
            {
                spdlog::error("Transformation name is empty");
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            if(!seen_names.insert(transformation_name).second)
            {
                spdlog::error("Duplicate transformation name in batch for `{}`", transformation_name);
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            normalizeForRegistry(record);

            const auto existing_record_handle_opt = _store->getTransformationRecordHandle(transformation_name);
            if(existing_record_handle_opt.has_value() && *existing_record_handle_opt)
            {
                if(!recordsEqual(*(*existing_record_handle_opt), record))
                {
                    spdlog::error(
                        "Transformation `{}` already exists with different payload",
                        transformation_name);
                    all_valid = false;
                    if(all_or_nothing)
                    {
                        co_return false;
                    }
                    continue;
                }

                putHotCacheEntry(
                    _transformation_record_cache,
                    transformation_name,
                    existing_record_handle_opt);
                continue;
            }

            batch_items.push_back(TransformationBatchItem{
                .address = address,
                .record = std::move(record)
            });
        }

        if(batch_items.empty())
        {
            co_return all_valid;
        }

        const bool inserted = _store->addTransformationsBatch(batch_items, all_or_nothing);
        if(inserted || !all_or_nothing)
        {
            for(const auto & item : batch_items)
            {
                if(!_store->hasTransformation(item.record.transformation().name()))
                {
                    continue;
                }
                putHotCacheEntry(
                    _transformation_record_cache,
                    item.record.transformation().name(),
                    std::optional<TransformationRecordHandle>(
                        std::make_shared<TransformationRecord>(item.record)));
            }
        }

        co_return inserted && all_valid;
    }

    asio::awaitable<std::optional<TransformationRecordHandle>> Registry::getTransformationRecordHandle(
        const std::string & name) const
    {
        spdlog::debug("Registry::getTransformationRecordHandle('{}'): enter", name);

        co_await async::ensureOnStrand(_strand);
        
        spdlog::debug("Registry::getTransformationRecordHandle('{}'): on strand", name);

        if(const auto * cached = getHotCacheEntry(_transformation_record_cache, name))
        {
            spdlog::debug(
                "Registry::getTransformationRecordHandle('{}'): cache hit has_value={}",
                name,
                cached->has_value());
            
            co_return *cached;
        }

        const auto record_handle_opt = _store->getTransformationRecordHandle(name);
            
        spdlog::debug(
            "Registry::getTransformationRecordHandle('{}'): store result has_value={}, has_record={}",
            name,
            record_handle_opt.has_value(),
            record_handle_opt.has_value() ? static_cast<bool>(*record_handle_opt) : false);

        putHotCacheEntry(_transformation_record_cache, name, record_handle_opt);
        
        spdlog::debug("Registry::getTransformationRecordHandle('{}'): done", name);
        co_return record_handle_opt;
    }

    asio::awaitable<bool> Registry::hasTransformation(const std::string & name) const
    {
        spdlog::debug("Registry::hasTransformation('{}'): enter", name);

        co_await async::ensureOnStrand(_strand);
        
        spdlog::debug("Registry::hasTransformation('{}'): on strand", name);

        if(const auto * cached = getHotCacheEntry(_transformation_record_cache, name))
        {
            const bool cache_has_record = cached->has_value() && static_cast<bool>(cached->value());
            
            spdlog::debug("Registry::hasTransformation('{}'): cache hit has_record={}", name, cache_has_record);
            
            if(cache_has_record)
            {
                co_return true;
            }
        }

        const bool exists = _store->hasTransformation(name);
        
        spdlog::debug("Registry::hasTransformation('{}'): store result={}", name, exists);
        
        if(!exists)
        {
            putHotCacheEntry(_transformation_record_cache, name, std::nullopt);
        }
        co_return exists;
    }

    asio::awaitable<bool> Registry::addCondition(chain::Address address, ConditionRecord record)
    {
        co_return co_await addConditionVersion(EntitySource{}, chain::Address{}, std::move(record));
    }

    asio::awaitable<bool> Registry::addConditionVersion(
        EntitySource source,
        chain::Address address,
        ConditionRecord record)
    {
        const std::string condition_name = record.condition().name();
        if(condition_name.empty())
        {
            spdlog::error("Condition name is empty");
            co_return false;
        }

        co_await async::ensureOnStrand(_strand);

        normalizeForRegistry(record);

        const auto existing_record_handle_opt = _store->getConditionRecordHandle(condition_name);
        if(existing_record_handle_opt.has_value() && *existing_record_handle_opt && source.source_id == "local")
        {
            if(!recordsEqual(*(*existing_record_handle_opt), record))
            {
                spdlog::error(
                    "Condition `{}` already exists with different payload",
                    condition_name);
                co_return false;
            }

            putHotCacheEntry(
                _condition_record_cache,
                condition_name,
                existing_record_handle_opt);
            co_return true;
        }

        const bool inserted = _store->addConditionVersion(source, address, record);
        if(inserted)
        {
            putHotCacheEntry(
                _condition_record_cache,
                condition_name,
                std::optional<ConditionRecordHandle>(
                    std::make_shared<ConditionRecord>(record)));
        }

        co_return inserted;
    }

    asio::awaitable<bool> Registry::addConditionsBatch(
        std::vector<std::pair<chain::Address, ConditionRecord>> conditions,
        bool all_or_nothing)
    {
        co_await async::ensureOnStrand(_strand);

        std::vector<ConditionBatchItem> batch_items;
        batch_items.reserve(conditions.size());
        absl::flat_hash_set<std::string> seen_names;
        seen_names.reserve(conditions.size());
        bool all_valid = true;
        for(auto & [address, record] : conditions)
        {
            const std::string condition_name = record.condition().name();
            if(condition_name.empty())
            {
                spdlog::error("Condition name is empty");
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            normalizeForRegistry(record);

            if(!seen_names.insert(condition_name).second)
            {
                spdlog::error("Duplicate condition name in batch for `{}`", condition_name);
                all_valid = false;
                if(all_or_nothing)
                {
                    co_return false;
                }
                continue;
            }

            const auto existing_record_handle_opt = _store->getConditionRecordHandle(condition_name);
            if(existing_record_handle_opt.has_value() && *existing_record_handle_opt)
            {
                if(!recordsEqual(*(*existing_record_handle_opt), record))
                {
                    spdlog::error(
                        "Condition `{}` already exists with different payload",
                        condition_name);
                    all_valid = false;
                    if(all_or_nothing)
                    {
                        co_return false;
                    }
                    continue;
                }

                putHotCacheEntry(
                    _condition_record_cache,
                    condition_name,
                    existing_record_handle_opt);
                continue;
            }

            batch_items.push_back(ConditionBatchItem{
                .address = address,
                .record = std::move(record)
            });
        }

        if(batch_items.empty())
        {
            co_return all_valid;
        }

        const bool inserted = _store->addConditionsBatch(batch_items, all_or_nothing);
        if(inserted || !all_or_nothing)
        {
            for(const auto & item : batch_items)
            {
                if(!_store->hasCondition(item.record.condition().name()))
                {
                    continue;
                }
                putHotCacheEntry(
                    _condition_record_cache,
                    item.record.condition().name(),
                    std::optional<ConditionRecordHandle>(
                        std::make_shared<ConditionRecord>(item.record)));
            }
        }

        co_return inserted && all_valid;
    }

    asio::awaitable<std::optional<ConditionRecordHandle>> Registry::getConditionRecordHandle(
        const std::string & name) const
    {
        spdlog::debug("Registry::getConditionRecordHandle('{}'): enter", name);

        co_await async::ensureOnStrand(_strand);
        
        spdlog::debug("Registry::getConditionRecordHandle('{}'): on strand", name);

        if(const auto * cached = getHotCacheEntry(_condition_record_cache, name))
        {
            spdlog::debug("Registry::getConditionRecordHandle('{}'): cache hit has_value={}", name, cached->has_value());
            co_return *cached;
        }

        const auto record_handle_opt = _store->getConditionRecordHandle(name);
        
        spdlog::debug(
            "Registry::getConditionRecordHandle('{}'): store result has_value={}, has_record={}",
            name,
            record_handle_opt.has_value(),
            record_handle_opt.has_value() ? static_cast<bool>(*record_handle_opt) : false);
        
        putHotCacheEntry(_condition_record_cache, name, record_handle_opt);
        
        spdlog::debug("Registry::getConditionRecordHandle('{}'): done", name);
        
        co_return record_handle_opt;
    }

    asio::awaitable<bool> Registry::hasCondition(const std::string & name) const
    {
        spdlog::debug("Registry::hasCondition('{}'): enter", name);

        co_await async::ensureOnStrand(_strand);
        
        spdlog::debug("Registry::hasCondition('{}'): on strand", name);

        if(const auto * cached = getHotCacheEntry(_condition_record_cache, name))
        {
            const bool cache_has_record = cached->has_value() && static_cast<bool>(cached->value());
            
            spdlog::debug("Registry::hasCondition('{}'): cache hit has_record={}", name, cache_has_record);
            
            if(cache_has_record)
            {
                co_return true;
            }
        }

        const bool exists = _store->hasCondition(name);
        
        spdlog::debug("Registry::hasCondition('{}'): store result={}", name, exists);
        
        if(!exists)
        {
            putHotCacheEntry(_condition_record_cache, name, std::nullopt);
        }
        co_return exists;
    }

    asio::awaitable<NameCursorPage> Registry::getOwnedConnectorsCursor(
        const chain::Address & owner,
        const std::optional<NameCursor> & after,
        std::size_t limit) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getOwnedConnectorsCursor(owner, after, limit);
    }

    asio::awaitable<NameCursorPage> Registry::getOwnedTransformationsCursor(
        const chain::Address & owner,
        const std::optional<NameCursor> & after,
        std::size_t limit) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getOwnedTransformationsCursor(owner, after, limit);
    }

    asio::awaitable<NameCursorPage> Registry::getOwnedConditionsCursor(
        const chain::Address & owner,
        const std::optional<NameCursor> & after,
        std::size_t limit) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getOwnedConditionsCursor(owner, after, limit);
    }

    asio::awaitable<std::size_t> Registry::getAccountsCount() const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getAccountsCount();
    }

    asio::awaitable<NameCursorPage> Registry::getAccountsCursor(
        const std::optional<chain::Address> & after,
        std::size_t limit) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getAccountsCursor(after, limit);
    }

    asio::awaitable<storage::sqlite::WalCheckpointStats> Registry::checkpointWal(const storage::sqlite::WalCheckpointMode mode) const
    {
        co_await async::ensureOnStrand(_strand);

        storage::sqlite::WalCheckpointStats stats{};
        stats.ok = _store->checkpointWal(mode);
        
        co_return stats;
    }

    std::int64_t Registry::getMaterializationCursor() const
    {
        return _store->getMaterializationCursor();
    }

    asio::awaitable<bool> Registry::setMaterializationCursor(std::int64_t seq)
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->setMaterializationCursor(seq);
    }

    asio::awaitable<std::optional<EntityLocation>> Registry::getEntityLocation(
        const EntityKind kind,
        const std::string & name) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getEntityLocation(kind, name);
    }

    asio::awaitable<bool> Registry::hasRemoteEntity(const EntityKind kind, const std::string & name) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->hasRemoteEntity(kind, name);
    }

    asio::awaitable<bool> Registry::hasLocalEntity(const EntityKind kind, const std::string & name) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->hasLocalEntity(kind, name);
    }

    asio::awaitable<bool> Registry::removeEntityVersion(
        const EntityKind kind,
        std::string source_id,
        std::string name)
    {
        co_await async::ensureOnStrand(_strand);
        const bool removed = _store->removeEntityVersion(kind, source_id, name);
        if(removed)
        {
            _connector_record_cache.entries.erase(name);
            _transformation_record_cache.entries.erase(name);
            _condition_record_cache.entries.erase(name);
            _format_hash_cache.entries.erase(name);
        }
        co_return removed;
    }

    asio::awaitable<bool> Registry::changeEntityOwner(
        const EntityKind kind,
        std::string source_id,
        std::string name,
        chain::Address old_owner,
        chain::Address new_owner)
    {
        co_await async::ensureOnStrand(_strand);
        const bool changed = _store->changeEntityOwner(kind, source_id, name, old_owner, new_owner);
        if(changed)
        {
            _connector_record_cache.entries.erase(name);
            _transformation_record_cache.entries.erase(name);
            _condition_record_cache.entries.erase(name);
        }
        co_return changed;
    }

    asio::awaitable<std::vector<ArtifactReference>> Registry::getArtifactsDue(
        const std::int64_t now_ms,
        const std::size_t limit) const
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->getArtifactsDue(now_ms, limit);
    }

    asio::awaitable<bool> Registry::setArtifactState(
        const EntityKind kind,
        std::string source_id,
        std::string name,
        const pt::ArtifactState state,
        const std::int64_t retry_at_ms)
    {
        co_await async::ensureOnStrand(_strand);
        co_return _store->setArtifactState(kind, source_id, name, state, retry_at_ms);
    }
}
