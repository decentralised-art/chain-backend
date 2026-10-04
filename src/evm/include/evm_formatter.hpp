#pragma once

#include <format>
#include <string>
#include <sstream>
#include <iomanip>

// Undefine the conflicting macro
#ifdef interface
    #undef interface
#endif

#include <evmc/evmc.h>
#include <evmc/evmc.hpp>
#include <evmc/hex.hpp>

#ifndef interface
    #define interface __STRUCT__
#endif

namespace dcn
{

}

template <>
struct std::formatter<evmc_address> : std::formatter<std::string> {
    auto format(const evmc_address & addr, format_context& ctx) const {
        return formatter<std::string>::format(evmc::hex(evmc::address(addr)), ctx);
    }
};

template <>
struct std::formatter<evmc::address> : std::formatter<std::string> {
    auto format(const evmc::address & addr, format_context& ctx) const {
        return formatter<std::string>{}.format(evmc::hex(addr), ctx);
    }
};

template <>
struct std::formatter<evmc_status_code> : std::formatter<std::string> {
    auto format(const evmc_status_code & code, format_context& ctx) const {
        switch(code)
        {
            case EVMC_SUCCESS : return formatter<string>::format("Success", ctx);
            case EVMC_FAILURE : return formatter<string>::format("Failure", ctx);
            case EVMC_REVERT : return formatter<string>::format("Revert", ctx);
            case EVMC_OUT_OF_GAS : return formatter<string>::format("Out of Gas", ctx);
            case EVMC_INVALID_INSTRUCTION : return formatter<string>::format("Invalid Instruction", ctx);
            case EVMC_UNDEFINED_INSTRUCTION : return formatter<string>::format("Undefined Instruction", ctx);
            case EVMC_STACK_OVERFLOW : return formatter<string>::format("Stack Overflow", ctx);
            case EVMC_STACK_UNDERFLOW : return formatter<string>::format("Stack Underflow", ctx);
            case EVMC_BAD_JUMP_DESTINATION : return formatter<string>::format("Bad Jump Destination", ctx);
            case EVMC_INVALID_MEMORY_ACCESS : return formatter<string>::format("Invalid Memory Access", ctx);
            case EVMC_CALL_DEPTH_EXCEEDED : return formatter<string>::format("Call Depth Exceeded", ctx);
            case EVMC_STATIC_MODE_VIOLATION : return formatter<string>::format("Static Mode Violation", ctx);
            case EVMC_PRECOMPILE_FAILURE : return formatter<string>::format("Precompile Failure", ctx);
            case EVMC_CONTRACT_VALIDATION_FAILURE : return formatter<string>::format("Contract Validation Failure", ctx);
            case EVMC_ARGUMENT_OUT_OF_RANGE : return formatter<string>::format("Argument Out of Range", ctx);
            case EVMC_WASM_UNREACHABLE_INSTRUCTION : return formatter<string>::format("WASM Unreachable Instruction", ctx);
            case EVMC_WASM_TRAP : return formatter<string>::format("WASM Trap", ctx);
            case EVMC_INSUFFICIENT_BALANCE : return formatter<string>::format("Insufficient Balance", ctx);
            case EVMC_INTERNAL_ERROR : return formatter<string>::format("Internal Error", ctx);
            case EVMC_REJECTED : return formatter<string>::format("Rejected", ctx);
            case EVMC_OUT_OF_MEMORY : return formatter<string>::format("Out of Memory", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};

template <>
struct std::formatter<evmc_call_kind> : std::formatter<std::string> {
    auto format(const evmc_call_kind & kind, format_context& ctx) const {
        switch(kind)
        {
            case EVMC_CALL : return formatter<string>::format("Call", ctx);
            case EVMC_DELEGATECALL : return formatter<string>::format("Delegate Call", ctx);
            case EVMC_CALLCODE : return formatter<string>::format("Call code", ctx);
            case EVMC_CREATE : return formatter<string>::format("Create", ctx);
            case EVMC_CREATE2 : return formatter<string>::format("Create2", ctx);
            case EVMC_EOFCREATE : return formatter<string>::format("EOF Create", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};

template <>
struct std::formatter<evmc_flags> : std::formatter<std::string> {
    auto format(const evmc_flags & flags, format_context& ctx) const {
        switch(flags)
        {
            case EVMC_STATIC : return formatter<string>::format("Static", ctx);
            case EVMC_DELEGATED : return formatter<string>::format("Delegated", ctx);
        }
        return formatter<string>::format("Unknown", ctx);
    }
};

template <>
struct std::formatter<evmc::bytes32> : std::formatter<std::string> {
    auto format(const evmc::bytes32 & bytes, format_context& ctx) const {
        std::string hex_str = evmc::hex(bytes);
        return std::formatter<std::string>::format(hex_str, ctx);
    }
};



//TODO evmc_revision


