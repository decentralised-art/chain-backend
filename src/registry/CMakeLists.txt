include("${PROJECT_SOURCE_DIR}/cmake/add_module.cmake")

add_module(NAME "Registry"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Native"
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Async"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::PT"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Storage"
        "${PROJECT_PREFIX}::Events"
        "${PROJECT_PREFIX}::Rpc"

        spdlog::spdlog
        asio
        absl::hash
        absl::flat_hash_map
        evmc
        sqlite3
        nlohmann_json
)
