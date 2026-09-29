add_module(NAME "API"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Async"
        "${PROJECT_PREFIX}::Native"
        "${PROJECT_PREFIX}::Events"
        "${PROJECT_PREFIX}::Version"
        "${PROJECT_PREFIX}::Config"
        "${PROJECT_PREFIX}::PT"
        "${PROJECT_PREFIX}::HTTP"
        "${PROJECT_PREFIX}::Server"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::Auth"
        "${PROJECT_PREFIX}::EVM"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::Rpc"
        "${PROJECT_PREFIX}::Publish"
        "${PROJECT_PREFIX}::Loader"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Registry"
        "${PROJECT_PREFIX}::Feed"
        "${PROJECT_PREFIX}::Storage"

        spdlog::spdlog
        asio
        secp256k1
        jwt-cpp
        evmc
        evmone
        nlohmann_json
        sqlite3
)

