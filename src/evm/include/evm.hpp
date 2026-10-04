#pragma once

#include <array>
#include <chrono>
#include <map>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <string>
#include <cstring>
#include <filesystem>
#include <vector>
#include <expected>
#include <fstream>
#include <istream>
#include <optional>

// Undefine the conflicting macro
#ifdef interface
    #undef interface
#endif

#include <evmc/evmc.h>
#include <evmc/evmc.hpp>
#include <evmc/mocked_host.hpp>

#include <evmc/hex.hpp>

#include <evmone/evmone.h>

#ifndef interface
    #define interface __STRUCT__
#endif

#include <spdlog/spdlog.h>
#include <absl/container/flat_hash_map.h>

#include "async.hpp"
#include "crypto.hpp"
#include "chain.hpp"

#include "evm_storage.hpp"
#include "evm_formatter.hpp"

namespace dcn::evm
{
    /// The one set of solc settings every contract is compiled with and every artifact records.
    struct CompilerSettings
    {
        std::string_view evm_version;
        bool optimizer = true;
        std::uint32_t optimizer_runs = 200;
        bool via_ir = true;
    };

    inline constexpr CompilerSettings SOLC_SETTINGS{
        .evm_version = "shanghai",
        .optimizer = true,
        .optimizer_runs = 200,
        .via_ir = true};

    /// Upper bound on one solc run; a compilation still running then is killed.
    inline constexpr std::chrono::seconds SOLC_TIMEOUT{120};

    struct CompileError
    {
        enum class Kind : std::uint8_t
        {
            // solc ran and did not build the contract: compiler errors, a crash, no usable
            // output or SOLC_TIMEOUT. The same source fails the same way again.
            REJECTED,
            // The compilation could not be carried out here: the work directory, the source
            // file or the solc process could not be set up. Worth retrying.
            FAILED
        };

        Kind kind = Kind::FAILED;
        std::string message;
    };

    struct CompiledContract
    {
        std::vector<std::uint8_t> creation_code;
        std::vector<std::uint8_t> runtime_code;

        // Every source unit the compilation read, by stable unit name, with the keccak256
        // solc recorded for it in the contract metadata.
        std::map<std::string, std::string> sources;
    };

    class EVM
    {
    public:
        using EmittedLogRecord = EVMStorage::EmittedLogRecord;

        EVM(asio::io_context & io_context, evmc_revision rev, std::filesystem::path solc_path, std::filesystem::path pt_path);
        ~EVM();

        EVM(const EVM&) = delete;
        EVM& operator=(const EVM&) = delete;

        EVM(EVM&&) = delete;
        EVM& operator=(EVM&&) = delete;

        asio::awaitable<bool> addAccount(chain::Address address, std::uint64_t initial_gas) noexcept;
        asio::awaitable<bool> setGas(chain::Address address, std::uint64_t gas) noexcept;

        // Compiles one file with SOLC_SETTINGS. solc runs on a dedicated thread, one
        // compilation at a time, so the io_context keeps serving while it works.
        asio::awaitable<bool> compile(std::filesystem::path code_path,
                std::filesystem::path out_dir,
                std::filesystem::path base_path = {},
                std::filesystem::path include_path = {}) const noexcept;

        // Compiles `source` as the source unit `unit_name` against the PT sources and returns
        // `contract_name`'s code. The unit sits on an include path, so its name carries no
        // machine-specific directory: the bytecode metadata, and with it the bytecode, is
        // the same wherever the source is compiled.
        asio::awaitable<std::expected<CompiledContract, CompileError>> compileSource(
                std::string unit_name,
                std::string source,
                std::string contract_name,
                std::filesystem::path work_dir) const;

        // Exact compiler version, e.g. "0.8.30+commit.73712a01"; empty when it could not be read.
        const std::string & solcVersion() const;

        asio::awaitable<std::expected<chain::Address, chain::DeployError>> deploy(  
                    std::istream & code_stream, 
                    chain::Address sender,
                    std::vector<std::uint8_t> constructor_args,
                    std::uint64_t gas_limit,
                    std::uint64_t value) noexcept;

        asio::awaitable<std::expected<chain::Address, chain::DeployError>> deploy(
                    std::filesystem::path code_path,    
                    chain::Address sender,
                    std::vector<uint8_t> constructor_args,
                    std::uint64_t gas_limit,
                    std::uint64_t value) noexcept;

        asio::awaitable<std::expected<std::vector<std::uint8_t>, chain::ExecuteError>> execute(
                    chain::Address sender,
                    chain::Address recipient, 
                    std::vector<std::uint8_t> input_bytes,
                    std::uint64_t gas_limit,
                    std::uint64_t value) noexcept;

        asio::awaitable<std::vector<EmittedLogRecord>> getLogsSince(std::uint64_t after_seq, std::size_t limit) noexcept;
        asio::awaitable<std::int64_t> getHeadBlockNumber() noexcept;

        chain::Address getRegistryAddress() const;
        chain::Address getRunnerAddress() const;

        const std::filesystem::path & getSolcPath() const;
        const std::filesystem::path & getPTPath() const;

        // Where this instance compiled the PT contracts it deployed at startup: a directory
        // of its own, so EVM instances sharing one PT path never touch each other's output.
        const std::filesystem::path & getPTOutPath() const;

    protected:
        asio::awaitable<bool> loadPT();

    private:
        asio::strand<asio::io_context::executor_type> _strand;
        
        evmc::VM _vm;
        evmc_revision _rev;

        std::filesystem::path _solc_path;
        std::filesystem::path _pt_path;
        std::filesystem::path _pt_out_path;
        std::string _solc_version;

        mutable asio::thread_pool _compile_pool{1};

        EVMStorage _storage;
        
        chain::Address _genesis_address;
        chain::Address _console_log_address;

        chain::Address _registry_address;
        chain::Address _runner_address;
    };

    asio::awaitable<std::expected<std::vector<std::uint8_t>, chain::ExecuteError>> fetchOwner(EVM & evm, const chain::Address & address);

    template<class T>
    std::vector<std::uint8_t> encodeAsArg(const T & val);

    template<>
    std::vector<std::uint8_t> encodeAsArg<chain::Address>(const chain::Address & address);

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::uint32_t>(const std::uint32_t & value);

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::vector<std::uint32_t>>(const std::vector<std::uint32_t> & vec);

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::vector<std::tuple<std::uint32_t, std::uint32_t>>>(const std::vector<std::tuple<std::uint32_t, std::uint32_t>>& vec);

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>>(const std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>& vec);

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::string>(const std::string& str);
}
