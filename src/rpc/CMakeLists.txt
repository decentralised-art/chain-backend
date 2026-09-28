include("${PROJECT_SOURCE_DIR}/cmake/add_module.cmake")

add_module(NAME "Rpc"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::PT"

        spdlog::spdlog
        asio
        evmc
        nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
)

# The TLS transport imports the Windows root certificate store.
if(WIN32)
    target_link_libraries("Rpc" PRIVATE crypt32)
endif()
