include("${PROJECT_SOURCE_DIR}/cmake/add_module.cmake")

add_module(NAME "Events"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Native"
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Async"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::Rpc"
        "${PROJECT_PREFIX}::PT"
        "${PROJECT_PREFIX}::EVM"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::Storage"

        absl::hash
        absl::flat_hash_map
        evmc
        evmone
        spdlog::spdlog
        asio
        nlohmann_json
        sqlite3
)
