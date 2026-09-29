add_module(NAME "EVM"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Native"
        "${PROJECT_PREFIX}::Async"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::Utils"

        absl::hash
        absl::flat_hash_map
        spdlog::spdlog
        asio
        evmc
        evmone
        secp256k1
        nlohmann_json
        protobuf::libprotobuf
)

add_dependencies("EVM" solc_imported evmone)
target_compile_definitions("EVM" PUBLIC Solidity_SOLC_EXECUTABLE=${Solidity_SOLC_EXECUTABLE})
