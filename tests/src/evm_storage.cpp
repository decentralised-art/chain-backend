#include "unit-tests.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include <evmone/evmone.h>

#include "evm_storage.hpp"

using namespace dcn;
using namespace dcn::tests;

namespace
{
    // With calldata: SSTORE(0, 1), LOG0, then REVERT. Without: returns SLOAD(0).
    constexpr std::array<std::uint8_t, 32> SLOT_RUNTIME{
        0x36, 0x15, 0x60, 0x14, 0x57,                       // no calldata: jump to the read
        0x60, 0x01, 0x60, 0x00, 0x55,                       // SSTORE(0, 1)
        0x60, 0x00, 0x60, 0x00, 0xa0,                       // LOG0(0, 0)
        0x60, 0x00, 0x60, 0x00, 0xfd,                       // REVERT(0, 0)
        0x5b, 0x60, 0x00, 0x54,                             // JUMPDEST, SLOAD(0)
        0x60, 0x00, 0x52, 0x60, 0x20, 0x60, 0x00, 0xf3};    // MSTORE(0, value), RETURN(0, 32)

    // Returns the SLOT_RUNTIME that follows it as the contract's code.
    constexpr std::array<std::uint8_t, 12> SLOT_INIT{
        0x60, 0x20, 0x60, 0x0c, 0x60, 0x00, 0x39,           // CODECOPY(0, 12, 32)
        0x60, 0x20, 0x60, 0x00, 0xf3};                      // RETURN(0, 32)

    // With calldata: CREATE an empty contract, then REVERT. Without: CREATE one and
    // return its address.
    constexpr std::array<std::uint8_t, 34> FACTORY_RUNTIME{
        0x36, 0x15, 0x60, 0x12, 0x57,                       // no calldata: jump to the returning create
        0x60, 0x00, 0x60, 0x00, 0x60, 0x00, 0xf0, 0x50,     // POP(CREATE(0, 0, 0))
        0x60, 0x00, 0x60, 0x00, 0xfd,                       // REVERT(0, 0)
        0x5b, 0x60, 0x00, 0x60, 0x00, 0x60, 0x00, 0xf0,     // JUMPDEST, CREATE(0, 0, 0)
        0x60, 0x00, 0x52, 0x60, 0x20, 0x60, 0x00, 0xf3};    // MSTORE(0, address), RETURN(0, 32)

    // Returns the FACTORY_RUNTIME that follows it as the contract's code.
    constexpr std::array<std::uint8_t, 12> FACTORY_INIT{
        0x60, 0x22, 0x60, 0x0c, 0x60, 0x00, 0x39,           // CODECOPY(0, 12, 34)
        0x60, 0x22, 0x60, 0x00, 0xf3};                      // RETURN(0, 34)

    // A creation whose init code fails: REVERT(0, 0).
    constexpr std::array<std::uint8_t, 5> FAILING_INIT{0x60, 0x00, 0x60, 0x00, 0xfd};

    constexpr std::array<std::uint8_t, 1> ANY_CALLDATA{0x01};

    template<std::size_t INIT_SIZE, std::size_t RUNTIME_SIZE>
    std::vector<std::uint8_t> creationCode(
        const std::array<std::uint8_t, INIT_SIZE> & init,
        const std::array<std::uint8_t, RUNTIME_SIZE> & runtime)
    {
        std::vector<std::uint8_t> code(init.begin(), init.end());
        code.insert(code.end(), runtime.begin(), runtime.end());
        return code;
    }

    evmc::address sender()
    {
        evmc::address address{};
        address.bytes[19] = 0x01;
        return address;
    }

    evmc::Result create(evm::EVMStorage & storage, const std::span<const std::uint8_t> code)
    {
        evmc_message message{};
        message.kind = EVMC_CREATE;
        message.sender = sender();
        message.gas = 1'000'000;
        message.input_data = code.data();
        message.input_size = code.size();
        return storage.call(message);
    }

    evmc::Result call(evm::EVMStorage & storage, const evmc::address & contract, const std::span<const std::uint8_t> input)
    {
        evmc_message message{};
        message.kind = EVMC_CALL;
        message.sender = sender();
        message.recipient = contract;
        message.gas = 1'000'000;
        message.input_data = input.empty() ? nullptr : input.data();
        message.input_size = input.size();
        return storage.call(message);
    }

    evmc::address returnedAddress(const evmc::Result & result)
    {
        evmc::address address{};
        if(result.output_size == 32)
        {
            std::copy_n(result.output_data + 12, 20, address.bytes);
        }
        return address;
    }
}

// A frame that fails must leave no trace, or a guard it set (such as a reentrancy lock)
// would stay set for every later transaction.
TEST_F(UnitTest, EVM_Storage_FailedFrameLeavesNoTrace)
{
    evmc::VM vm{evmc_create_evmone()};
    evm::EVMStorage storage(vm, EVMC_SHANGHAI);
    ASSERT_TRUE(storage.add_account(sender()));

    const evmc::Result created = create(storage, creationCode(SLOT_INIT, SLOT_RUNTIME));
    ASSERT_EQ(created.status_code, EVMC_SUCCESS);
    const evmc::address contract = created.create_address;

    EXPECT_EQ(call(storage, contract, ANY_CALLDATA).status_code, EVMC_REVERT);

    EXPECT_EQ(storage.get_storage(contract, evmc::bytes32{}), evmc::bytes32{});
    EXPECT_TRUE(storage.get_logs_since(0, 16).empty());

    // The contract itself reads the slot as never written.
    const evmc::Result read = call(storage, contract, {});
    ASSERT_EQ(read.status_code, EVMC_SUCCESS);
    ASSERT_EQ(read.output_size, 32u);
    EXPECT_TRUE(std::all_of(read.output_data, read.output_data + read.output_size, [](const std::uint8_t b) { return b == 0; }));
}

// A creator whose own frame fails gets back the nonce its CREATE spent, so its next
// creation lands where it would have without the failed frame.
TEST_F(UnitTest, EVM_Storage_FailedCreatorFrameUndoesItsNonceBump)
{
    evmc::VM vm{evmc_create_evmone()};

    evm::EVMStorage untouched(vm, EVMC_SHANGHAI);
    const evmc::Result untouched_factory = create(untouched, creationCode(FACTORY_INIT, FACTORY_RUNTIME));
    ASSERT_EQ(untouched_factory.status_code, EVMC_SUCCESS);
    const evmc::Result expected = call(untouched, untouched_factory.create_address, {});
    ASSERT_EQ(expected.status_code, EVMC_SUCCESS);

    evm::EVMStorage storage(vm, EVMC_SHANGHAI);
    const evmc::Result factory = create(storage, creationCode(FACTORY_INIT, FACTORY_RUNTIME));
    ASSERT_EQ(factory.status_code, EVMC_SUCCESS);
    ASSERT_EQ(evmc::address{factory.create_address}, evmc::address{untouched_factory.create_address});
    EXPECT_EQ(call(storage, factory.create_address, ANY_CALLDATA).status_code, EVMC_REVERT);

    const evmc::Result created = call(storage, factory.create_address, {});
    ASSERT_EQ(created.status_code, EVMC_SUCCESS);
    EXPECT_NE(returnedAddress(created), evmc::address{});
    EXPECT_EQ(returnedAddress(created), returnedAddress(expected));
}

// A creation that fails still spends its creator's nonce, as on a chain.
TEST_F(UnitTest, EVM_Storage_FailedCreationStillSpendsTheCreatorsNonce)
{
    evmc::VM vm{evmc_create_evmone()};

    evm::EVMStorage untouched(vm, EVMC_SHANGHAI);
    ASSERT_EQ(create(untouched, creationCode(FACTORY_INIT, FACTORY_RUNTIME)).status_code, EVMC_SUCCESS);
    const evmc::Result second = create(untouched, creationCode(FACTORY_INIT, FACTORY_RUNTIME));
    ASSERT_EQ(second.status_code, EVMC_SUCCESS);

    evm::EVMStorage storage(vm, EVMC_SHANGHAI);
    EXPECT_EQ(create(storage, FAILING_INIT).status_code, EVMC_REVERT);
    const evmc::Result after_failure = create(storage, creationCode(FACTORY_INIT, FACTORY_RUNTIME));
    ASSERT_EQ(after_failure.status_code, EVMC_SUCCESS);
    EXPECT_EQ(evmc::address{after_failure.create_address}, evmc::address{second.create_address});
}
