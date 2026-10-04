add_module(NAME "Auth"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Native"
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::HTTP"
        "${PROJECT_PREFIX}::Async"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::Parser"

        spdlog::spdlog
        asio
        absl::hash
        secp256k1
        jwt-cpp
        evmc
        nlohmann_json
        protobuf::libprotobuf
)

