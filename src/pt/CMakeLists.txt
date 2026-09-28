include("${PROJECT_SOURCE_DIR}/cmake/add_module.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/protobuf_generate.cmake")

# Create PT target
add_module(NAME "PT"
    DEPENDENCIES
        "${PROJECT_PREFIX}::Native"
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::File"

        absl::hash
        spdlog::spdlog
        asio
        evmc
        secp256k1
        nlohmann_json
)

add_dependencies("PT" check_protoc_version pt_configure)

protobuf_generate(NAME "pt_proto_gen" 
    PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTO_DST_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries("PT" PUBLIC "pt_proto_gen")