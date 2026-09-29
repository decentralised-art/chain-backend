#include "evm_storage.hpp"
#include "evm.hpp"

#include "keccak256.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <ctime>
#include <limits>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

namespace dcn::evm
{
    namespace
    {
        constexpr std::uint64_t FIRST_CREATE_NONCE = 100;
        constexpr std::int64_t ECRECOVER_GAS = 3000;

        bool _isEcrecover(const evmc::address& address)
        {
            evmc::address ecrecover{};
            ecrecover.bytes[19] = 0x01;
            return address == ecrecover;
        }

        // The ecrecover precompile: hash, v, r and s as four words (short input is zero
        // padded); answers the signer as one word, or nothing when none can be recovered.
        // Unlike signature verification it accepts a high s, as a chain does.
        evmc::Result _ecrecover(const evmc_message& msg)
        {
            if(msg.gas < ECRECOVER_GAS) return evmc::Result{EVMC_OUT_OF_GAS};
            const std::int64_t gas_left = msg.gas - ECRECOVER_GAS;

            std::array<std::uint8_t, 128> input{};
            if(msg.input_data != nullptr)
            {
                std::copy_n(msg.input_data, std::min(msg.input_size, input.size()), input.begin());
            }

            const bool v_valid = std::all_of(input.begin() + 32, input.begin() + 63, [](const std::uint8_t b) { return b == 0; })
                && (input[63] == 27 || input[63] == 28);
            if(!v_valid) return evmc::Result{EVMC_SUCCESS, gas_left, 0, nullptr, 0};

            const secp256k1_context* ctx = secp256k1_context_static;
            secp256k1_ecdsa_recoverable_signature signature;
            secp256k1_pubkey public_key;
            if(!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &signature, input.data() + 64, input[63] - 27)
                || !secp256k1_ecdsa_recover(ctx, &public_key, &signature, input.data()))
            {
                return evmc::Result{EVMC_SUCCESS, gas_left, 0, nullptr, 0};
            }

            std::array<std::uint8_t, 65> serialized{};
            std::size_t length = serialized.size();
            secp256k1_ec_pubkey_serialize(ctx, serialized.data(), &length, &public_key, SECP256K1_EC_UNCOMPRESSED);
            const chain::Address signer = chain::ethAddressFromPublicKey(serialized.data(), length);

            std::array<std::uint8_t, 32> word{};
            std::copy(std::begin(signer.bytes), std::end(signer.bytes), word.begin() + 12);
            return evmc::Result{EVMC_SUCCESS, gas_left, 0, word.data(), word.size()};
        }
    }

    EVMStorage::EVMStorage(evmc::VM & vm, evmc_revision rev)
        :   _vm(vm),
            _revision(rev)
    {
    }

    evmc::bytes32 EVMStorage::make_pseudo_hash(
        const std::uint64_t value_a,
        const std::uint64_t value_b,
        const std::uint64_t value_c) const
    {
        std::array<std::uint8_t, sizeof(value_a) + sizeof(value_b) + sizeof(value_c)> payload{};
        std::memcpy(payload.data(), &value_a, sizeof(value_a));
        std::memcpy(payload.data() + sizeof(value_a), &value_b, sizeof(value_b));
        std::memcpy(payload.data() + sizeof(value_a) + sizeof(value_b), &value_c, sizeof(value_c));

        evmc::bytes32 hash{};
        dcn::crypto::Keccak256::getHash(payload.data(), payload.size(), hash.bytes);
        return hash;
    }

    bool EVMStorage::account_exists(const evmc::address& addr) const noexcept
    {
        return _accounts.find(to_key(addr)) != _accounts.end();
    }

    evmc::bytes32 EVMStorage::get_storage(const evmc::address& addr, const evmc::bytes32& key) const noexcept
    {
        if(account_exists(addr) == false) 
        {
            spdlog::error(std::format("get_storage : Account {} does not exist", addr));
            return {};
        }
        if(_accounts.at(to_key(addr)).storage.find(key) == _accounts.at(to_key(addr)).storage.end()) return {};
        return _accounts.at(to_key(addr)).storage.at(key);
    }

    evmc_storage_status EVMStorage::set_storage(const evmc::address& address, const evmc::bytes32& key, const evmc::bytes32& value) noexcept
    {
        const std::string account_key = to_key(address);
        if(_journaling())
        {
            const auto account = _accounts.find(account_key);
            if(account == _accounts.end())
            {
                _recordAccount(account_key);
            }
            else
            {
                const auto slot = account->second.storage.find(key);
                _journal.push_back(SlotChange{
                    .account = account_key,
                    .key = key,
                    .previous = slot == account->second.storage.end()
                        ? std::nullopt
                        : std::optional<evmc::bytes32>(slot->second)});
            }
        }
        _accounts[account_key].storage[key] = value;
        return EVMC_STORAGE_MODIFIED;
    }

    void EVMStorage::_recordAccount(const std::string& account)
    {
        if(!_journaling()) return;
        const auto it = _accounts.find(account);
        _journal.push_back(AccountChange{
            .account = account,
            .previous = it == _accounts.end() ? std::nullopt : std::optional<Account>(it->second)});
    }

    std::uint64_t EVMStorage::_spendCreateNonce(const evmc::address& creator)
    {
        const auto it = _create_nonce.find(creator);
        const std::optional<std::uint64_t> previous =
            it == _create_nonce.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
        if(_journaling())
        {
            _journal.push_back(NonceChange{.creator = creator, .previous = previous});
        }

        const std::uint64_t nonce = previous.value_or(FIRST_CREATE_NONCE);
        _create_nonce[creator] = nonce + 1;
        return nonce;
    }

    EVMStorage::Checkpoint EVMStorage::_checkpoint() const noexcept
    {
        return Checkpoint{
            .journal_size = _journal.size(),
            .emitted_logs = _emitted_logs.size(),
            .next_log_seq = _next_log_seq,
            .next_log_index = _tx_context_stack.empty() ? 0 : _tx_context_stack.back().next_log_index};
    }

    void EVMStorage::_revertTo(const Checkpoint& checkpoint)
    {
        // Newest first, so every record restores the state it saw.
        while(_journal.size() > checkpoint.journal_size)
        {
            auto & entry = _journal.back();
            if(auto * slot = std::get_if<SlotChange>(&entry))
            {
                auto & storage = _accounts[slot->account].storage;
                if(slot->previous) storage[slot->key] = *slot->previous;
                else storage.erase(slot->key);
            }
            else if(auto * account = std::get_if<AccountChange>(&entry))
            {
                if(account->previous) _accounts[account->account] = std::move(*account->previous);
                else _accounts.erase(account->account);
            }
            else
            {
                auto & nonce = std::get<NonceChange>(entry);
                if(nonce.previous) _create_nonce[nonce.creator] = *nonce.previous;
                else _create_nonce.erase(nonce.creator);
            }
            _journal.pop_back();
        }

        _emitted_logs.erase(
            _emitted_logs.begin() + static_cast<std::ptrdiff_t>(checkpoint.emitted_logs), _emitted_logs.end());
        _next_log_seq = checkpoint.next_log_seq;
        if(!_tx_context_stack.empty())
        {
            _tx_context_stack.back().next_log_index = checkpoint.next_log_index;
        }
    }

    evmc::uint256be EVMStorage::get_balance(const evmc::address& addr) const noexcept
    {
        if(account_exists(addr) == false) 
        {
            spdlog::error(std::format("get_balance : Account {} does not exist", addr));
            return {};
        }
        return evmc::uint256be{_accounts.at(to_key(addr)).balance};
    }

    std::size_t EVMStorage::get_code_size(const evmc::address& addr) const noexcept
    {
        return _accounts.at(to_key(addr)).code.size();
    }

    evmc::bytes32 EVMStorage::get_code_hash(const evmc::address& addr) const noexcept
    {
        if(account_exists(addr) == false) 
        {
            spdlog::error(std::format("get_code_hash : Account {} does not exist", addr));
            return {};
        }

        const auto& code = _accounts.at(to_key(addr)).code;
        evmc::bytes32 hash;
        dcn::crypto::Keccak256::getHash((const std::uint8_t*)code.data(), code.size(), hash.bytes);
        return hash;
    }

    std::size_t EVMStorage::copy_code(const evmc::address& addr,
                             std::size_t code_offset,
                             std::uint8_t* buffer_data,
                             std::size_t buffer_size) const noexcept
    {
        spdlog::debug(std::format("copy_code : Account {}, offset {}, buffer_size {}", addr, code_offset, buffer_size));
        const auto& code = _accounts.at(to_key(addr)).code;
        if (code_offset >= code.size())
        {
            spdlog::error(std::format("copy_code : Invalid code offset: {}", code_offset));
            return 0;
        }
        std::size_t copy_size = std::min(buffer_size, code.size() - code_offset);
        std::memcpy(buffer_data, code.data() + code_offset, copy_size);
        return copy_size;
    }

    bool EVMStorage::selfdestruct(const evmc::address& addr, const evmc::address& beneficiary) noexcept
    {
        auto key = to_key(addr);
        auto it = _accounts.find(key);
        if (it == _accounts.end()) return false;

        // Transfer balance
        // implement evmc_uint256be operations
        //_accounts[to_key(beneficiary)].balance += it->second.balance;
        if(_journaling())
        {
            _journal.push_back(AccountChange{.account = key, .previous = std::move(it->second)});
        }
        _accounts.erase(it);
        return true;
    }

    evmc::Result EVMStorage::call(const evmc_message& msg) noexcept
    {
        const bool top_level_call = _sender_stack.empty();
        evmc::address actual_sender = msg.sender;

        // Patch: if sender is zero and we are in a nested context
        if (evmc::is_zero(msg.sender) && !_sender_stack.empty()) {
            actual_sender = _sender_stack.top();
        }
        // Push current sender onto the stack
        _sender_stack.push(actual_sender);
        struct SenderStackScopeGuard
        {
            std::stack<evmc::address> & stack;
            ~SenderStackScopeGuard()
            {
                if(!stack.empty())
                {
                    stack.pop();
                }
            }
        };
        SenderStackScopeGuard sender_stack_scope_guard{_sender_stack};

        if(top_level_call)
        {
            ActiveTxContext tx{};
            tx.block_number = ++_head_block_number;
            tx.block_time = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            tx.parent_hash = _head_block_hash;
            tx.block_hash = make_pseudo_hash(
                static_cast<std::uint64_t>(tx.block_number),
                0,
                0);
            tx.tx_hash = make_pseudo_hash(
                static_cast<std::uint64_t>(tx.block_number),
                _next_tx_id,
                1);
            tx.tx_index = 0;
            tx.next_log_index = 0;

            _head_block_hash = tx.block_hash;
            ++_next_tx_id;
            _tx_context_stack.push_back(std::move(tx));
        }

        struct TxContextScopeGuard
        {
            std::vector<ActiveTxContext> & stack;
            bool pop_on_exit = false;
            ~TxContextScopeGuard()
            {
                if(pop_on_exit && !stack.empty())
                {
                    stack.pop_back();
                }
            }
        };
        TxContextScopeGuard tx_context_scope_guard{_tx_context_stack, top_level_call};

        // CREATE spends its creator's nonce in the creator's frame, ahead of the new frame's
        // checkpoint: a failed creation keeps the bump, as on a chain, and only a failure
        // of the creator's own frame undoes it.
        const std::uint64_t create_nonce = msg.kind == EVMC_CREATE ? _spendCreateNonce(actual_sender) : 0;

        const Checkpoint checkpoint = _checkpoint();
        evmc::Result result = _dispatch(msg, actual_sender, create_nonce);
        if(result.status_code != EVMC_SUCCESS)
        {
            _revertTo(checkpoint);
        }
        if(top_level_call)
        {
            // The transaction has ended, so nothing can revert its changes any more.
            _journal.clear();
        }
        return result;
    }

    evmc::Result EVMStorage::_dispatch(
        const evmc_message& msg,
        const evmc::address& actual_sender,
        const std::uint64_t create_nonce) noexcept
    {
        if (msg.kind == EVMC_CALL || msg.kind == EVMC_DELEGATECALL || msg.kind == EVMC_CALLCODE)
        {
            evmc::address code_address = msg.recipient;
            if (msg.kind == EVMC_DELEGATECALL || msg.kind == EVMC_CALLCODE)
            {
                code_address = msg.code_address;
            }

            // ponytail: ecrecover is the only precompile served, since PT verifies owner
            // signatures with it; serve others once a contract calls one.
            if(_isEcrecover(code_address))
            {
                return _ecrecover(msg);
            }

            auto it = _accounts.find(to_key(code_address));

            if (it == _accounts.end())
            {
                spdlog::error(std::format("call: Code account {} does not exist", code_address));
                return evmc::Result{EVMC_FAILURE};
            }

            auto& code = it->second.code;
            if (code.empty())
            {
                spdlog::error(std::format("call: Code account {} has no code (recipient {})", code_address, msg.recipient));
                return evmc::Result{EVMC_FAILURE};
            }
            
            evmc_message patched_msg = msg;
            //patched_msg.sender = actual_sender;
            evmc::Result result = _vm.execute(*this, _revision, patched_msg, code.data(), code.size());

            if (result.status_code != EVMC_SUCCESS)
            {
                std::string output_hex = "<empty>";
                if(result.output_data != nullptr && result.output_size > 0)
                {
                    output_hex = evmc::hex(evmc::bytes_view{result.output_data, result.output_size});
                }

                spdlog::error(std::format("call: Failed to execute contract: {} {}", result.status_code, output_hex));
                return result;
            }

            spdlog::debug(std::format("call: EVMC execute call from {} to {} ended", actual_sender, msg.recipient));

            return result;
        }
        else if (msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2)
        {
            assert(msg.recipient == evmc::address{});
            evmc_message patched_msg = msg;

            const std::vector<std::uint8_t> init_code(patched_msg.input_data, patched_msg.input_data + patched_msg.input_size);

            // Calculate address
            evmc_address new_address;
            if (patched_msg.kind == EVMC_CREATE)
            {
                new_address = derive_create_address(actual_sender, create_nonce);
            }
            else // CREATE2
            {
                new_address = derive_create2_address(actual_sender, patched_msg.create2_salt, init_code);
            }

            add_account(new_address);
            set_balance(new_address, DEFAULT_GAS_LIMIT);

            // at this point constructor does not have any address - so any operation - for example store ect 
            // will use default 0x0 address, therefore we use dummy ctor account, 
            // to which constructor code can write its sload, sstore operations ect
            patched_msg.recipient = new_address;

            //patched_msg.sender = actual_sender;
            evmc::Result result = _vm.execute(*this, _revision, patched_msg, init_code.data(), init_code.size());

            if (result.status_code != EVMC_SUCCESS)
            {
                std::string output_hex = "<empty>";
                if(result.output_data != nullptr && result.output_size > 0)
                {
                    output_hex = evmc::hex(evmc::bytes_view{result.output_data, result.output_size});
                }

                spdlog::error(std::format("call: Failed to deploy contract: {} {}", result.status_code, output_hex));
                return result;
            }

            std::vector<std::uint8_t> deployed_code;
            if(result.output_data != nullptr && result.output_size > 0)
            {
                deployed_code.assign(result.output_data, result.output_data + result.output_size);
            }

            deploy_contract(new_address, std::move(deployed_code),
                patched_msg.value, actual_sender, create_nonce);

            result.create_address = new_address;
            
            spdlog::debug(std::format("call: EVMC create call from {} to {} ended", actual_sender, new_address));
            return result;
        }

        spdlog::error(std::format("call: Unsupported message kind: {}", msg.kind));
        return evmc::Result{EVMC_FAILURE};
    }

    evmc_tx_context EVMStorage::get_tx_context() const noexcept
    {
        evmc_tx_context ctx{};

        // Set a dummy gas price 
        //ctx.tx_gas_price.bytes[31] = 1;

        // Set the origin (e.g. zero address or some predefined test sender)
        ctx.tx_origin = {}; // All zeroes by default

        // Block metadata
        ctx.block_coinbase = {};                   // Miner address (can be zero)
        if(!_tx_context_stack.empty())
        {
            const auto & tx = _tx_context_stack.back();
            ctx.block_number = tx.block_number;
            ctx.block_timestamp = tx.block_time;
        }
        else
        {
            ctx.block_number = _head_block_number;
            ctx.block_timestamp = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
        }
        ctx.block_gas_limit = DEFAULT_GAS_LIMIT;   // gas limit
        ctx.block_prev_randao = {};                // Randomness (EIP-4399)
        
        ctx.block_base_fee = {};
        //ctx.block_base_fee.bytes[31] = 1;          // Base fee (EIP-1559)

        // Chain ID (mainnet = 1)
        //ctx.chain_id.bytes[31] = 1;

        // Leave EIP-4844 blob fields null
        ctx.blob_hashes = nullptr;
        ctx.blob_hashes_count = 0;

        // Leave EIP-7685 TXCREATE initcodes empty
        ctx.initcodes = nullptr;
        ctx.initcodes_count = 0;

        return ctx;
    }

    evmc::bytes32 EVMStorage::get_block_hash(int64_t block_number) const noexcept
    {
        if(block_number <= 0 || block_number > _head_block_number)
        {
            return {};
        }

        return make_pseudo_hash(
            static_cast<std::uint64_t>(block_number),
            0,
            0);
    }

    static bool _decodeAbiWordToSize(const std::uint8_t * word, std::size_t & out)
    {
        constexpr std::size_t kWordSize = 32;
        constexpr std::size_t kValueBytes = sizeof(std::size_t);
        static_assert(kValueBytes <= kWordSize);

        // Reject values that do not fit into size_t.
        for(std::size_t i = 0; i < (kWordSize - kValueBytes); ++i)
        {
            if(word[i] != 0)
            {
                return false;
            }
        }

        out = 0;
        for(std::size_t i = kWordSize - kValueBytes; i < kWordSize; ++i)
        {
            out = (out << 8) | static_cast<std::size_t>(word[i]);
        }

        return true;
    }

    static std::string _decodeString(const std::uint8_t * data, std::size_t data_size, std::size_t offset)
    {
        constexpr std::size_t kWordSize = 32;
        if(offset > data_size || (data_size - offset) < kWordSize)
        {
            return "<out-of-bounds>";
        }

        std::size_t len = 0;
        if(!_decodeAbiWordToSize(data + offset, len))
        {
            return "<out-of-bounds>";
        }

        const std::size_t payload_offset = offset + kWordSize;
        if(len > (data_size - payload_offset))
        {
            return "<out-of-bounds>";
        }

        return std::string(reinterpret_cast<const char *>(data + payload_offset), len);
    }


    void EVMStorage::emit_log(const evmc::address& addr,
                              const std::uint8_t* data,
                              size_t data_size,
                              const evmc::bytes32 topics[],
                              size_t num_topics) noexcept
    {
        if(!_tx_context_stack.empty())
        {
            auto & tx = _tx_context_stack.back();

            EmittedLogRecord row{};
            row.seq = _next_log_seq++;
            row.block_number = tx.block_number;
            row.block_hash = evmc::hex(tx.block_hash);
            row.parent_hash = evmc::hex(tx.parent_hash);
            row.tx_index = tx.tx_index;
            row.tx_hash = evmc::hex(tx.tx_hash);
            row.log_index = tx.next_log_index++;
            row.address = evmc::hex(addr);
            row.block_time = tx.block_time;
            row.topics.reserve(num_topics);

            for(std::size_t i = 0; i < num_topics; ++i)
            {
                row.topics.push_back(evmc::hex(topics[i]));
            }

            if(data != nullptr && data_size > 0)
            {
                row.data_hex = evmc::hex(evmc::bytes_view{data, data_size});
            }

            _emitted_logs.push_back(std::move(row));
        }

        // const auto connector_added_event = decodeConnectorAddedEvent(data, data_size, topics, num_topics);
        // if(connector_added_event)
        // {
        //     std::string composite_names_str;
        //     for(std::size_t i = 0; i < connector_added_event->composite_names.size(); ++i)
        //     {
        //         const auto & composite_name = connector_added_event->composite_names[i];
        //         composite_names_str += std::format("    [{}] {}\n", i, composite_name.length() > 0 ? composite_name : "<none>");
        //     }

        //     std::string condition_args_str;
        //     for(std::size_t i = 0; i < connector_added_event->condition_args.size(); ++i)
        //     {
        //         condition_args_str += std::format("    [{}]: {}\n", i, connector_added_event->condition_args[i]);
        //     }

        //     spdlog::info(std::format(
        //         "Decoded PT event ConnectorAdded from {}:\n"
        //         "  Caller: {}\n"
        //         "  Owner: {}\n"
        //         "  Name: {}\n"
        //         "  Connector: {}\n"
        //         "  Feature: {}\n"
        //         "  Condition: {}\n"
        //         "  CompositeNames:\n{}"
        //         "  ConditionArgs:\n{}",
        //         addr,
        //         connector_added_event->caller,
        //         connector_added_event->owner,
        //         connector_added_event->name,
        //         connector_added_event->connector_address,
        //         connector_added_event->feature_name,
        //         connector_added_event->condition_name.empty() ? "    <none>\n" : connector_added_event->condition_name,
        //         composite_names_str.empty() ? "    <none>\n" : composite_names_str,
        //         condition_args_str.empty() ? "    <none>\n" : condition_args_str
        //     )
        //     );
        //     return;
        // }

        // const auto feature_added_event = decodeFeatureAddedEvent(data, data_size, topics, num_topics);
        // if(feature_added_event)
        // {
        //     spdlog::info(std::format(
        //         "Decoded PT event FeatureAdded from {}:\n"
        //         "  Caller: {}\n"
        //         "  Owner: {}\n"
        //         "  Name: {}\n"
        //         "  Feature: {}\n"
        //         "  DimensionsCount: {}",
        //         addr,
        //         feature_added_event->caller,
        //         feature_added_event->owner,
        //         feature_added_event->name,
        //         feature_added_event->feature_address,
        //         feature_added_event->dimensions_count
        //     ));
        //     return;
        // }

        // const auto transformation_added_event = decodeTransformationAddedEvent(data, data_size, topics, num_topics);
        // if(transformation_added_event)
        // {
        //     spdlog::info(std::format(
        //         "Decoded PT event TransformationAdded from {}:\n"
        //         "  Caller: {}\n"
        //         "  Owner: {}\n"
        //         "  Name: {}\n"
        //         "  Transformation: {}\n"
        //         "  ArgsCount: {}",
        //         addr,
        //         transformation_added_event->caller,
        //         transformation_added_event->owner,
        //         transformation_added_event->name,
        //         transformation_added_event->transformation_address,
        //         transformation_added_event->args_count
        //     ));
        //     return;
        // }

        // const auto condition_added_event = decodeConditionAddedEvent(data, data_size, topics, num_topics);
        // if(condition_added_event)
        // {
        //     spdlog::info(std::format(
        //         "Decoded PT event ConditionAdded from {}:\n"
        //         "  Caller: {}\n"
        //         "  Owner: {}\n"
        //         "  Name: {}\n"
        //         "  Condition: {}\n"
        //         "  ArgsCount: {}",
        //         addr,
        //         condition_added_event->caller,
        //         condition_added_event->owner,
        //         condition_added_event->name,
        //         condition_added_event->condition_address,
        //         condition_added_event->args_count
        //     ));
        //     return;
        // }

        std::string label = "<n/a>";
        std::string value = "<n/a>";

        if(data_size >= 64)
        {
            std::size_t offset_label = 0;
            std::size_t offset_value = 0;
            if(_decodeAbiWordToSize(data, offset_label) && _decodeAbiWordToSize(data + 32, offset_value))
            {
                label = _decodeString(data, data_size, offset_label);
                value = _decodeString(data, data_size, offset_value);
            }
            else
            {
                label = "<out-of-bounds>";
                value = "<out-of-bounds>";
            }
        }

        // Format topics
        std::string topics_str;
        for (size_t i = 0; i < num_topics; ++i)
            topics_str += std::format("Topic[{}]: {}\n", i, topics[i]);

        spdlog::info(std::format("Log from {}:\n  Data: {} bytes\n  Label: {}\n  Value: {}\n  Topics:\n{}",
                                 addr, data_size, label, value, topics_str));
    }

    evmc_access_status EVMStorage::access_account(const evmc::address& addr) noexcept
    {
        if (account_exists(addr))
            return EVMC_ACCESS_WARM;
        return EVMC_ACCESS_COLD;
    }

    evmc_access_status EVMStorage::access_storage(const evmc::address& addr, const evmc::bytes32& key) noexcept
    {
        if (account_exists(addr))
            return EVMC_ACCESS_WARM;
        return EVMC_ACCESS_COLD;
    }

    evmc::bytes32 EVMStorage::get_transient_storage(const evmc::address& addr,
                                          const evmc::bytes32& key) const noexcept
    {
        // implement
        assert(false && "not implemented");

        return {};
    }

    void EVMStorage::set_transient_storage(const evmc::address& addr,
                                       const evmc::bytes32& key,
                                       const evmc::bytes32& value) noexcept
    {
        // implement
        assert(false && "not implemented");
    }

    std::vector<EVMStorage::EmittedLogRecord> EVMStorage::get_logs_since(
        const std::uint64_t after_seq,
        const std::size_t limit) const
    {
        std::vector<EmittedLogRecord> out;
        if(limit == 0 || _emitted_logs.empty())
        {
            return out;
        }
        if(after_seq == std::numeric_limits<std::uint64_t>::max())
        {
            return out;
        }

        const auto it = std::lower_bound(
            _emitted_logs.begin(),
            _emitted_logs.end(),
            after_seq + 1,
            [](const EmittedLogRecord & row, const std::uint64_t seq)
            {
                return row.seq < seq;
            });

        const std::size_t available = static_cast<std::size_t>(_emitted_logs.end() - it);
        const std::size_t count = std::min(limit, available);
        out.reserve(count);
        for(std::size_t i = 0; i < count; ++i)
        {
            out.push_back(*(it + static_cast<std::ptrdiff_t>(i)));
        }
        return out;
    }

    std::int64_t EVMStorage::head_block_number() const noexcept
    {
        return _head_block_number;
    }


    std::string EVMStorage::to_key(const evmc::address& addr) const
    {
        return std::string(reinterpret_cast<const char*>(addr.bytes), sizeof(addr.bytes));
    }

    evmc_address EVMStorage::derive_create_address(const evmc::address& sender, std::uint64_t nonce)
    {
        std::array<std::uint8_t, 64> data{};
        std::memcpy(data.data(), sender.bytes, 20);
        std::memcpy(data.data() + 20, &nonce, sizeof(nonce));
            
        evmc::bytes32 hash;
        dcn::crypto::Keccak256::getHash((const std::uint8_t*)data.data(), 20 + sizeof(nonce), hash.bytes);

        evmc_address addr;
        std::memcpy(addr.bytes, &hash.bytes[12], 20);
        return addr;
    }

    evmc_address EVMStorage::derive_create2_address(const evmc::address& sender, const evmc::bytes32& salt, const std::vector<std::uint8_t>& code)
    {
        std::vector<std::uint8_t> data;
        data.reserve(1 + 20 + 32 + 32);
        data.push_back(0xff);
        data.insert(data.end(), sender.bytes, sender.bytes + 20);
        data.insert(data.end(), salt.bytes, salt.bytes + 32);

        evmc::bytes32 code_hash;
        dcn::crypto::Keccak256::getHash((const std::uint8_t*)code.data(), code.size(), code_hash.bytes);

        data.insert(data.end(), code_hash.bytes, code_hash.bytes + 32);

        evmc::bytes32 hash;
        dcn::crypto::Keccak256::getHash((const std::uint8_t*)data.data(), data.size(), hash.bytes);

        evmc_address addr;
        std::memcpy(addr.bytes, &hash.bytes[12], 20);
        return addr;
    }

    void EVMStorage::deploy_contract(evmc::address addr,
                    std::vector<std::uint8_t>&& code,
                    evmc_uint256be value,
                    evmc::address creator,
                    std::uint64_t nonce)
    {
        if(account_exists(addr) == false) 
        {
            spdlog::error(std::format("deploy_contract: Account {} does not exists", addr));
            return;
        }

        spdlog::debug(std::format("Deploying contract to {} by {}", addr,creator));
        _recordAccount(to_key(addr));
        auto& account = _accounts[to_key(addr)];
        account.code = std::move(code);
        account.balance = value;
        account.creator = creator;
        account.nonce = nonce;
        account.timestamp = static_cast<std::uint64_t>(std::chrono::seconds(std::time(nullptr)).count());
    }
}
