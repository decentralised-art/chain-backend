#include <exception>
#include <limits>
#include <string_view>

#include "evm.hpp"

#include <cctype>
#include <regex>

#include <nlohmann/json.hpp>
#include "utils.hpp"

namespace dcn::evm
{
    namespace
    {
        std::vector<std::string> _solcArguments(
            const std::filesystem::path & out_dir,
            const std::filesystem::path & base_path,
            const std::vector<std::filesystem::path> & include_paths)
        {
            const CompilerSettings & settings = SOLC_SETTINGS;
            std::vector<std::string> args{
                "--evm-version", std::string(settings.evm_version),
                "--overwrite", "-o", out_dir.string(),
                "--bin", "--bin-runtime", "--abi", "--metadata"};

            if(settings.via_ir)
            {
                args.emplace_back("--via-ir");
            }
            if(settings.optimizer)
            {
                args.emplace_back("--optimize");
                args.emplace_back("--optimize-runs");
                args.emplace_back(std::to_string(settings.optimizer_runs));
            }
            if(!base_path.empty())
            {
                args.emplace_back("--base-path");
                args.emplace_back(base_path.string());
            }
            for(const auto & include_path : include_paths)
            {
                args.emplace_back("--include-path");
                args.emplace_back(include_path.string());
            }
            return args;
        }

        std::pair<int, std::string> _runSolc(const std::filesystem::path & solc_path, std::vector<std::string> args)
        {
            try
            {
                return native::runProcess(solc_path.string(), std::move(args), SOLC_TIMEOUT);
            }
            catch(const std::exception & error)
            {
                return {-1, error.what()};
            }
        }

        std::string _readSolcVersion(const std::filesystem::path & solc_path)
        {
            const auto [exit_code, output] = _runSolc(solc_path, {"--version"});
            static const std::regex pattern(R"((\d+\.\d+\.\d+\+commit\.[0-9a-fA-F]+))");
            std::smatch match;
            if(exit_code == 0 && std::regex_search(output, match, pattern))
            {
                return match[1].str();
            }
            spdlog::error("Cannot determine the solc version of '{}'", solc_path.string());
            return {};
        }

        std::optional<std::string> _readFile(const std::filesystem::path & path)
        {
            std::ifstream input(path, std::ios::binary);
            if(!input.is_open())
            {
                return std::nullopt;
            }
            return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        }

        std::optional<std::vector<std::uint8_t>> _readHexFile(const std::filesystem::path & path)
        {
            auto text = _readFile(path);
            if(!text)
            {
                return std::nullopt;
            }
            std::erase_if(*text, [](const unsigned char c) { return std::isspace(c) != 0; });
            auto bytes = evmc::from_hex(*text);
            if(!bytes)
            {
                return std::nullopt;
            }
            return std::vector<std::uint8_t>(bytes->begin(), bytes->end());
        }
    }

    namespace
    {
        std::vector<std::uint8_t> copyResultBytes(const evmc::Result & result)
        {
            if(result.output_data == nullptr || result.output_size == 0)
            {
                return {};
            }

            return std::vector<std::uint8_t>(result.output_data, result.output_data + result.output_size);
        }
    }

    template<>
    std::vector<std::uint8_t> encodeAsArg<chain::Address>(const chain::Address & address)
    {
        std::vector<std::uint8_t> encoded(32, 0); // Initialize with 32 zero bytes
        std::copy(address.bytes, address.bytes + 20, encoded.begin() + 12); // Right-align in last 20 bytes
        return encoded;
    }

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::uint32_t>(const std::uint32_t & value)
    {
        std::vector<std::uint8_t> encoded(32, 0); // Initialize with 32 zero bytes

        // Encode as big-endian and place in the last 4 bytes (right-aligned)
        encoded[28] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
        encoded[29] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        encoded[30] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        encoded[31] = static_cast<std::uint8_t>(value & 0xFF);

        return encoded;
    }

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::vector<std::uint32_t>>(const std::vector<std::uint32_t>& vec)
    {
        std::vector<std::uint8_t> encoded;

        // Step 1: offset to the data section (0x20 = 32)
        encoded.resize(32, 0);
        encoded[31] = 0x20; // offset is 32 bytes

        // Step 2: dynamic data section begins
        std::vector<std::uint8_t> data;

        // 2.1: encode length (number of elements)
        data.resize(32, 0);
        std::uint32_t length = static_cast<std::uint32_t>(vec.size());
        data[28] = static_cast<std::uint8_t>((length >> 24) & 0xFF);
        data[29] = static_cast<std::uint8_t>((length >> 16) & 0xFF);
        data[30] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
        data[31] = static_cast<std::uint8_t>(length & 0xFF);

        // 2.2: encode each element (right-aligned uint32 in 32 bytes)
        for (const std::uint32_t val : vec)
        {
            std::vector<std::uint8_t> element(32, 0);
            element[28] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
            element[29] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
            element[30] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
            element[31] = static_cast<std::uint8_t>(val & 0xFF);
            data.insert(data.end(), element.begin(), element.end());
        }

        // Step 3: append the data section after the 32-byte offset
        encoded.insert(encoded.end(), data.begin(), data.end());

        return encoded;
    }

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::vector<std::tuple<std::uint32_t, std::uint32_t>>>(const std::vector<std::tuple<std::uint32_t, std::uint32_t>>& vec)
    {
        std::vector<std::uint8_t> encoded;

        // Encode length
        std::vector<std::uint8_t> length(32, 0);
        uint64_t len = vec.size();
        for (int i = 0; i < 8; ++i)
            length[31 - i] = static_cast<uint8_t>(len >> (i * 8));
        
        encoded.insert(encoded.end(), length.begin(), length.end());

        // Encode each pair as two int32s in a 32-byte ABI word each
        for (const auto& [a, b] : vec)
        {
            std::vector<std::uint8_t> elem(32, 0);
            for (int i = 0; i < 4; ++i)
                elem[31 - i] = static_cast<uint8_t>(a >> (i * 8));
            
            encoded.insert(encoded.end(), elem.begin(), elem.end());

            std::fill(elem.begin(), elem.end(), 0);
            for (int i = 0; i < 4; ++i)
                elem[31 - i] = static_cast<uint8_t>(b >> (i * 8));
            
            encoded.insert(encoded.end(), elem.begin(), elem.end());
        }

        return encoded;
    }

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>>(const std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>& vec)
    {
        std::vector<std::uint8_t> encoded;

        std::vector<std::uint8_t> length(32, 0);
        uint64_t len = vec.size();
        for(int i = 0; i < 8; ++i)
        {
            length[31 - i] = static_cast<std::uint8_t>(len >> (i * 8));
        }
        encoded.insert(encoded.end(), length.begin(), length.end());

        for(const auto & [a, b, c] : vec)
        {
            std::vector<std::uint8_t> elem(32, 0);

            for(int i = 0; i < 4; ++i)
            {
                elem[31 - i] = static_cast<std::uint8_t>(a >> (i * 8));
            }
            encoded.insert(encoded.end(), elem.begin(), elem.end());

            std::fill(elem.begin(), elem.end(), 0);
            for(int i = 0; i < 4; ++i)
            {
                elem[31 - i] = static_cast<std::uint8_t>(b >> (i * 8));
            }
            encoded.insert(encoded.end(), elem.begin(), elem.end());

            std::fill(elem.begin(), elem.end(), 0);
            for(int i = 0; i < 4; ++i)
            {
                elem[31 - i] = static_cast<std::uint8_t>(c >> (i * 8));
            }
            encoded.insert(encoded.end(), elem.begin(), elem.end());
        }

        return encoded;
    }

    template<>
    std::vector<std::uint8_t> encodeAsArg<std::string>(const std::string& str)
    {
        std::vector<std::uint8_t> encoded;

        std::vector<uint8_t> len_enc(32, 0);
        const std::size_t length = str.size();
        for(std::size_t i = 0; i < sizeof(length); ++i)
        {
            len_enc[31 - i] = static_cast<std::uint8_t>((length >> (8 * i)) & 0xFF);
        }

        encoded.insert(encoded.end(), len_enc.begin(), len_enc.end()); // string length
        encoded.insert(encoded.end(), str.begin(), str.end());             // string bytes

        // pad to multiple of 32 bytes
        size_t pad = (32 - (str.size() % 32)) % 32;
        encoded.insert(encoded.end(), pad, 0);
        
        return encoded;
    }

    asio::awaitable<std::expected<std::vector<std::uint8_t>, chain::ExecuteError>> fetchOwner(EVM & evm, const chain::Address & address)
    {
        spdlog::debug(std::format("Fetching contract owner: {}", address));
        std::vector<uint8_t> input_data;
        const auto selector = chain::constructSelector("getOwner()");
        input_data.insert(input_data.end(), selector.begin(), selector.end());
        co_return co_await evm.execute(evm.getRegistryAddress(), address, input_data, 1'000'000, 0);
    }

    EVM::EVM(asio::io_context & io_context, evmc_revision rev, std::filesystem::path solc_path, std::filesystem::path pt_path)
    :   _vm(evmc_create_evmone()),
        _rev(rev),
        _strand(asio::make_strand(io_context)),
        _solc_path(std::move(solc_path)),
        _pt_path(std::move(pt_path)),
        _storage(_vm, _rev)
    {
        if (!_vm)
        {
            throw std::runtime_error("Failed to create EVM instance");
        }

        _solc_version = _readSolcVersion(_solc_path);

        _vm.set_option("O", "0"); // disable optimizations

        // Initialize the genesis account
        std::memcpy(_genesis_address.bytes + (20 - 7), "genesis", 7);
        addAccount(_genesis_address, DEFAULT_GAS_LIMIT);
        spdlog::info(std::format("Genesis address: {}", _genesis_address));

        // Initialize console log account
        std::memcpy(_console_log_address.bytes + (20 - 11), "console.log", 11);
        addAccount(_console_log_address, DEFAULT_GAS_LIMIT);

        co_spawn(
            io_context,
            loadPT(),
            [&io_context](std::exception_ptr exception_ptr, bool loaded_ok)
            {
                if(exception_ptr)
                {
                    utils::logException(exception_ptr, "PT loader coroutine failed");
                    spdlog::critical("Stopping io_context because PT failed to load");
                    io_context.stop();
                    return;
                }

                if(!loaded_ok)
                {
                    spdlog::critical("PT failed to load");
                    spdlog::critical("Stopping io_context because PT failed to load");
                    io_context.stop();
                }
            });
    }
    
    chain::Address EVM::getRegistryAddress() const
    {
        return _registry_address;
    }

    chain::Address EVM::getRunnerAddress() const
    {
        return _runner_address;
    }

    const std::filesystem::path & EVM::getSolcPath() const 
    {
        return _solc_path;
    }

    const std::filesystem::path & EVM::getPTPath() const 
    {
        return _pt_path;
    }

    asio::awaitable<bool> EVM::addAccount(chain::Address address, std::uint64_t initial_gas) noexcept
    {
        co_await async::ensureOnStrand(_strand);

        if(_storage.account_exists(address))
        {
            spdlog::warn(std::format("addAccount: Account {} already exists", evmc::hex(address)));
            co_return false;
        }

        if(_storage.add_account(address))
        {
            _storage.set_balance(address, initial_gas);
        }
        else
        {
            co_return false;
        }

        co_return true;
    }

    asio::awaitable<bool> EVM::setGas(chain::Address address, std::uint64_t gas) noexcept
    {
        co_await async::ensureOnStrand(_strand);

        if(!_storage.account_exists(address))
        {
            spdlog::warn(std::format("addAccount: Account {} does not exist", evmc::hex(address)));
            co_return false;
        }

        _storage.set_balance(address, gas);
        co_return true;
    }

    const std::string & EVM::solcVersion() const
    {
        return _solc_version;
    }

    asio::awaitable<bool> EVM::compile(
        std::filesystem::path code_path,
        std::filesystem::path out_dir,
        std::filesystem::path base_path,
        std::vector<std::filesystem::path> include_paths) const noexcept
    {
        std::error_code exists_ec;
        if(!std::filesystem::exists(code_path, exists_ec))
        {
            spdlog::error(std::format("File {} does not exist", code_path.string()));
            co_return false;
        }

        if(!include_paths.empty() && base_path.empty())
        {
            spdlog::error("Base path must be specified if includes are specified");
            co_return false;
        }

        std::vector<std::string> args = _solcArguments(out_dir, base_path, include_paths);
        args.push_back(code_path.string());

        const auto [exit_code, compile_result] = co_await asio::co_spawn(
            _compile_pool,
            [solc = _solc_path, args = std::move(args)]() mutable -> asio::awaitable<std::pair<int, std::string>>
            {
                co_return _runSolc(solc, std::move(args));
            },
            asio::use_awaitable);

        spdlog::info("Solc exited with code {},\n{}\n{}", exit_code, code_path.string(), compile_result);

        co_return exit_code == 0;
    }

    asio::awaitable<std::expected<CompiledContract, std::string>> EVM::compileSource(
        std::string unit_name,
        std::string source,
        std::string contract_name,
        std::filesystem::path work_dir) const
    {
        const std::filesystem::path solc = _solc_path;
        const std::filesystem::path pt_contracts = _pt_path / "contracts";
        const std::filesystem::path pt_modules = _pt_path / "node_modules";

        // Everything runs on the single compile thread, so two compilations never share a
        // source file or an output file.
        co_return co_await asio::co_spawn(
            _compile_pool,
            [=]() -> asio::awaitable<std::expected<CompiledContract, std::string>>
            {
                std::error_code ec;
                std::filesystem::create_directories(work_dir, ec);
                if(ec)
                {
                    co_return std::unexpected(std::format("cannot create '{}': {}", work_dir.string(), ec.message()));
                }

                const std::filesystem::path source_path = work_dir / unit_name;
                {
                    std::ofstream output(source_path, std::ios::binary | std::ios::trunc);
                    output << source;
                    if(!output)
                    {
                        co_return std::unexpected(std::format("cannot write '{}'", source_path.string()));
                    }
                }

                std::vector<std::string> args = _solcArguments(work_dir, pt_contracts, {pt_modules, work_dir});
                args.push_back(source_path.string());
                const auto [exit_code, output] = _runSolc(solc, std::move(args));
                std::filesystem::remove(source_path, ec);
                if(exit_code != 0)
                {
                    co_return std::unexpected(std::format("solc exited with {}: {}", exit_code, output));
                }

                auto creation_code = _readHexFile(work_dir / (contract_name + ".bin"));
                auto runtime_code = _readHexFile(work_dir / (contract_name + ".bin-runtime"));
                const auto metadata_text = _readFile(work_dir / (contract_name + "_meta.json"));
                if(!creation_code || !runtime_code || runtime_code->empty() || !metadata_text)
                {
                    co_return std::unexpected(std::format("solc produced no output for '{}'", contract_name));
                }

                const nlohmann::json metadata = nlohmann::json::parse(*metadata_text, nullptr, false);
                const auto sources_it = metadata.is_object() ? metadata.find("sources") : metadata.end();
                if(metadata.is_discarded() || sources_it == metadata.end() || !sources_it->is_object())
                {
                    co_return std::unexpected(std::format("solc metadata for '{}' lists no sources", contract_name));
                }

                CompiledContract compiled{
                    .creation_code = std::move(*creation_code),
                    .runtime_code = std::move(*runtime_code)};
                for(const auto & [unit, description] : sources_it->items())
                {
                    const auto hash = description.is_object() ? description.find("keccak256") : description.end();
                    if(hash == description.end() || !hash->is_string())
                    {
                        co_return std::unexpected(std::format("solc metadata has no hash for source '{}'", unit));
                    }
                    compiled.sources.emplace(unit, hash->get<std::string>());
                }

                if(!compiled.sources.contains(unit_name))
                {
                    co_return std::unexpected(std::format(
                        "solc did not record '{}' under its stable source unit name", unit_name));
                }

                co_return compiled;
            },
            asio::use_awaitable);
    }

    asio::awaitable<std::expected<chain::Address, chain::DeployError>> EVM::deploy(
                        std::istream & code_stream,
                        chain::Address sender,
                        std::vector<std::uint8_t> constructor_args, 
                        std::uint64_t gas_limit,
                        std::uint64_t value) noexcept
    {
        const std::string code_hex = std::string(std::istreambuf_iterator<char>(code_stream), std::istreambuf_iterator<char>());
        const std::optional<evmc::bytes> bytecode_result = evmc::from_hex(code_hex);
        if(!bytecode_result)
        {
            spdlog::error("Cannot parse bytecode");
            co_return std::unexpected(chain::DeployError{
                .kind = chain::DeployError::Kind::INVALID_INPUT,
                .message = "Cannot parse bytecode"
            });
        }
        const auto & bytecode = *bytecode_result;

        if(bytecode.size() == 0)
        {
            spdlog::error("Empty bytecode");
            co_return std::unexpected(chain::DeployError{
                .kind = chain::DeployError::Kind::INVALID_INPUT,
                .message = "Empty bytecode"
            });
        }

        if(!constructor_args.empty())
        {
            std::string hex_str;
            for(const std::uint8_t & b : constructor_args)
            {
                hex_str += evmc::hex(b);
            }

            spdlog::debug(std::format("Constructor args: {}", hex_str));
        }

        std::vector<uint8_t> deployment_input;
        deployment_input.reserve(bytecode.size() + constructor_args.size());
        deployment_input.insert(deployment_input.end(), bytecode.begin(), bytecode.end());
        deployment_input.insert(deployment_input.end(), constructor_args.begin(), constructor_args.end());

        evmc_message create_msg{};
        create_msg.kind       = EVMC_CREATE;
        create_msg.sender     = sender;
        std::memcpy(create_msg.sender.bytes, sender.bytes, 20);
        
        create_msg.gas        = gas_limit;
        create_msg.input_data = deployment_input.data();
        create_msg.input_size = deployment_input.size();

        // fill message value
        evmc_uint256be value256{};
        std::memcpy(&value256.bytes[24], &value, sizeof(value));  // Big endian: last 8 bytes hold the value
        create_msg.value = value256;

        co_await async::ensureOnStrand(_strand);

        const evmc::Result result = _storage.call(create_msg);        

        if (result.status_code != EVMC_SUCCESS)
        {
            const chain::DeployError error{
                .kind = (result.status_code == EVMC_REVERT)
                    ? chain::DeployError::Kind::TRANSACTION_REVERTED
                    : chain::DeployError::Kind::UNKNOWN,
                .message = std::format("Failed to deploy contract: {}", result.status_code),
                .result_bytes = copyResultBytes(result)
            };

            std::string output_hex = "<empty>";
            if(result.output_data != nullptr && result.output_size > 0)
            {
                output_hex = evmc::hex(evmc::bytes_view{result.output_data, result.output_size});
            }

            spdlog::error(std::format("Failed to deploy contract: {}, error: {} {}", result.status_code, error.kind, output_hex));
            co_return std::unexpected(error);
        }

        // Display result
        spdlog::info("EVM deployment status: {}", evmc_status_code_to_string(result.status_code));
        spdlog::info("Gas left: {}", result.gas_left);

        if (result.output_data){
            spdlog::debug("Output size: {}", result.output_size);
        }

        co_return result.create_address;
     }

    asio::awaitable<std::expected<chain::Address, chain::DeployError>> EVM::deploy(
                        std::filesystem::path code_path,
                        chain::Address sender,
                        std::vector<uint8_t> constructor_args,
                        std::uint64_t gas_limit,
                        std::uint64_t value) noexcept
    {
        spdlog::debug(std::format("Deploying contract from file: {}", code_path.string()));
        std::ifstream file(code_path, std::ios::binary);
        co_return co_await deploy(file, std::move(sender), std::move(constructor_args),  gas_limit, value);
    }

    asio::awaitable<std::expected<std::vector<std::uint8_t>, chain::ExecuteError>> EVM::execute(
                    chain::Address sender,
                    chain::Address recipient,
                    std::vector<std::uint8_t> input_bytes,
                    std::uint64_t gas_limit,
                    std::uint64_t value) noexcept
    {
        if(std::ranges::all_of(recipient.bytes, [](uint8_t b) { return b == 0; }))
        {
            spdlog::error("Cannot create a contract with execute function. Use dedicated deploy method.");
            co_return std::unexpected(chain::ExecuteError{
                .kind = chain::ExecuteError::Kind::INVALID_INPUT,
                .message = "Cannot create a contract with execute function. Use dedicated deploy method."
            });
        }

        evmc_message msg{};
        msg.gas = gas_limit;
        msg.kind = EVMC_CALL;
        msg.sender = sender;
        msg.recipient = recipient;

        if(!input_bytes.empty())
        {
            msg.input_data = input_bytes.data();
            msg.input_size = input_bytes.size();
        }
        else
        {
            msg.input_data = nullptr;
            msg.input_size = 0;
        }

        evmc_uint256be value256{};
        std::memcpy(&value256.bytes[24], &value, sizeof(value));  // Big endian: last 8 bytes hold the value
        msg.value = value256;

        co_await async::ensureOnStrand(_strand);
        evmc::Result result = _storage.call(msg);
        
        if (result.status_code != EVMC_SUCCESS)
        {
            const chain::ExecuteError error
            {
                .kind = chain::ExecuteError::Kind::TRANSACTION_REVERTED,
                .result_bytes = copyResultBytes(result)
            };

            std::string output_hex = "<empty>";
            if(result.output_data != nullptr && result.output_size > 0)
            {
                output_hex = evmc::hex(evmc::bytes_view{result.output_data, result.output_size});
            }

            spdlog::error(std::format("Failed to execute contract: {}, error: {} {}", result.status_code, error.kind, output_hex));
            co_return std::unexpected(error);
        }

        // Display result
        spdlog::info("EVM execution status: {}", evmc_status_code_to_string(result.status_code));
        spdlog::info("Gas left: {}", result.gas_left);

        if (result.output_data){
            spdlog::debug("Output size: {}", result.output_size);
        }

        co_return copyResultBytes(result);
    }

    asio::awaitable<std::vector<EVM::EmittedLogRecord>> EVM::getLogsSince(
        const std::uint64_t after_seq,
        const std::size_t limit) noexcept
    {
        co_await async::ensureOnStrand(_strand);
        co_return _storage.get_logs_since(after_seq, limit);
    }

    asio::awaitable<std::int64_t> EVM::getHeadBlockNumber() noexcept
    {
        co_await async::ensureOnStrand(_strand);
        co_return _storage.head_block_number();
    }

    asio::awaitable<bool> EVM::loadPT()
    { 
        const auto  contracts_dir = _pt_path    / "contracts";
        const auto node_modules = _pt_path      / "node_modules";
        const auto  out_dir = _pt_path          / "out";
        const auto proxy_out_dir = out_dir      / "proxy";

        std::filesystem::create_directories(out_dir);
        
        { // deploy registry implementation + proxy
            if(co_await compile(
                    contracts_dir / "registry" / "RegistryBase.sol",
                    out_dir / "registry", 
                    contracts_dir,
                    {node_modules}) == false)
            {
                spdlog::error("Failed to compile registry");
                co_return false;
            }

            if(co_await compile(
                    contracts_dir / "proxy" / "PTRegistryProxy.sol",
                    proxy_out_dir, 
                    contracts_dir,
                    {node_modules}) == false)
            {
                spdlog::error("Failed to compile registry proxy");
                co_return false;
            }

            const auto registry_impl_address_res = co_await deploy(
                    out_dir / "registry" / "RegistryBase.bin", 
                    _genesis_address,
                    {}, 
                    DEFAULT_GAS_LIMIT, 
                    0);

            if(!registry_impl_address_res)
                co_return false;

            const auto registry_impl_address = registry_impl_address_res.value();
            spdlog::info("Registry implementation address: {}", evmc::hex(registry_impl_address));

            const auto registry_proxy_address_res = co_await deploy(
                    proxy_out_dir / "PTRegistryProxy.bin",
                    _genesis_address,
                    encodeAsArg(registry_impl_address),
                    DEFAULT_GAS_LIMIT,
                    0);

            if(!registry_proxy_address_res)
                co_return false;

            _registry_address = registry_proxy_address_res.value();
            spdlog::info("Registry proxy address: {}", evmc::hex(_registry_address));
        }

        { // deploy runner implementation + proxy
            if(co_await compile(
                    contracts_dir / "runner" /  "Runner.sol",
                    out_dir / "runner", 
                    contracts_dir,
                    {node_modules}) == false)
            {
                spdlog::error("Failed to compile runner");
                co_return false;
            }

            if(co_await compile(
                    contracts_dir / "proxy" / "PTContractProxy.sol",
                    proxy_out_dir, 
                    contracts_dir,
                    {node_modules}) == false)
            {
                spdlog::error("Failed to compile contract proxy");
                co_return false;
            }
            
            spdlog::debug("Deploy runner implementation");
            const auto runner_impl_address_res = co_await deploy(
                    out_dir / "runner" / "Runner.bin", 
                    _genesis_address,
                    {}, 
                    DEFAULT_GAS_LIMIT, 
                    0);

            if(!runner_impl_address_res)
                co_return false;

            const auto runner_impl_address = runner_impl_address_res.value();
            spdlog::info("Runner implementation address: {}", evmc::hex(runner_impl_address));

            std::vector<std::uint8_t> runner_proxy_ctor_args = encodeAsArg(runner_impl_address);
            const auto registry_arg = encodeAsArg(_registry_address);
            runner_proxy_ctor_args.insert(runner_proxy_ctor_args.end(), registry_arg.begin(), registry_arg.end());

            spdlog::debug("Deploy runner proxy");
            const auto runner_proxy_address_res = co_await deploy(
                    proxy_out_dir / "PTContractProxy.bin",
                    _genesis_address,
                    std::move(runner_proxy_ctor_args),
                    DEFAULT_GAS_LIMIT,
                    0);

            if(!runner_proxy_address_res)
                co_return false;
        
            _runner_address = runner_proxy_address_res.value();
            spdlog::info("Runner proxy address: {}", evmc::hex(_runner_address));
        }

        co_return true;
    }

}
