include("${PROJECT_SOURCE_DIR}/cmake/add_module.cmake")

add_module(NAME "Publish"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Async"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::PT"
        "${PROJECT_PREFIX}::Rpc"
        "${PROJECT_PREFIX}::Events"
        "${PROJECT_PREFIX}::File"

        spdlog::spdlog
        asio
        evmc
        secp256k1
        nlohmann_json
)
