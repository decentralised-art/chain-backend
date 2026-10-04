add_module(NAME "Crypto"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Native"
        
        spdlog::spdlog
        asio
        absl::hash
        secp256k1
        jwt-cpp
        OpenSSL::Crypto
)
