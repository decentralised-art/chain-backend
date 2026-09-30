cmake_minimum_required(VERSION 3.30)

set(PROJECT_NAME "DecentralisedArtServer")
set(PROJECT_PREFIX "DecentralisedArt")

project("${PROJECT_NAME}"   
    VERSION "0.0.1"
    DESCRIPTION "decentralised.art chain backend"
    HOMEPAGE_URL "https://github.com/decentralised-art/chain-backend"
    LANGUAGES C CXX
)

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------
option(DECENTRALISED_ART_ENABLE_INSTALL "Enable install rule" OFF)
option(DECENTRALISED_ART_BUILD_TESTS "Build tests" OFF)
option(DECENTRALISED_ART_BUILD_STRESS_TESTS "Build stress test module" OFF)
option(DECENTRALISED_ART_USE_SUBMODULE_PT "Use submodule PT" OFF)

# --------------------------------------------------------------------------
set(CMAKE_INSTALL_MESSAGE "LAZY")
message(STATUS "Install directory ${CMAKE_INSTALL_PREFIX}")
set(CMAKE_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_PREFIX}/include")
set(CMAKE_INSTALL_LIBDIR "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_INSTALL_BINDIR "${CMAKE_INSTALL_PREFIX}/bin")
message(STATUS "include directory ${CMAKE_INSTALL_INCLUDEDIR}")
message(STATUS "lib directory ${CMAKE_INSTALL_LIBDIR}")
message(STATUS "bin directory ${CMAKE_INSTALL_BINDIR}")

include(cmake/compile_options.cmake)
include(cmake/dependencies.cmake)

add_subdirectory(src)

# create static library target
add_library("${PROJECT_NAME}Lib" STATIC "include/decentralised_art.hpp")

target_include_directories("${PROJECT_NAME}Lib"     
    PUBLIC 
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>")

target_link_libraries("${PROJECT_NAME}Lib"
    PUBLIC
        spdlog
        asio
        jwt-cpp
        secp256k1
        evmc
        evmone
        nlohmann_json
        sqlite3

        pt_proto_gen
        
        "${PROJECT_PREFIX}::Native"
        "${PROJECT_PREFIX}::Async"
        "${PROJECT_PREFIX}::Config"
        "${PROJECT_PREFIX}::CMD"
        "${PROJECT_PREFIX}::Version"
        "${PROJECT_PREFIX}::Utils"
        "${PROJECT_PREFIX}::Crypto"
        "${PROJECT_PREFIX}::Storage"
        "${PROJECT_PREFIX}::Events"
        "${PROJECT_PREFIX}::Feed"
        "${PROJECT_PREFIX}::Server"
        "${PROJECT_PREFIX}::HTTP"
        "${PROJECT_PREFIX}::PT"
        "${PROJECT_PREFIX}::Parser"
        "${PROJECT_PREFIX}::File"
        "${PROJECT_PREFIX}::Auth"
        "${PROJECT_PREFIX}::API"
        "${PROJECT_PREFIX}::EVM"
        "${PROJECT_PREFIX}::Loader"
        "${PROJECT_PREFIX}::Registry"
        "${PROJECT_PREFIX}::Chain"
        "${PROJECT_PREFIX}::Rpc"
        "${PROJECT_PREFIX}::Publish"
)

target_sources("${PROJECT_NAME}Lib" PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS include
    FILES "include/decentralised_art.hpp"
)

# static library install target
install(TARGETS ${PROJECT_NAME}Lib
    EXPORT ${PROJECT_NAME}LibTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET public_headers
)

# create executable target
add_executable("${PROJECT_NAME}" src/main.cpp)

target_link_libraries("${PROJECT_NAME}"
    PRIVATE
        "${PROJECT_NAME}Lib"
)

# sets the RPATH in the executable to search for libraries in a path relative to the executable location
if(APPLE)
    set_target_properties("${PROJECT_NAME}" PROPERTIES INSTALL_RPATH "@loader_path/../lib")
else()
    set_target_properties("${PROJECT_NAME}" PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

# executable install target
install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(MSVC)
    # Install debug symbols only for non-Release configs.
    install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        CONFIGURATIONS Debug RelWithDebInfo
        OPTIONAL
    )
elseif(APPLE OR UNIX)
    # For single-config generators, only generate debug-symbol sidecars
    # when building Debug/RelWithDebInfo. For multi-config generators,
    # keep the post-build generation and constrain installation by config.
    set(_install_debug_symbols FALSE)
    if(CMAKE_CONFIGURATION_TYPES)
        set(_install_debug_symbols TRUE)
    elseif(CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
        set(_install_debug_symbols TRUE)
    endif()

    if(_install_debug_symbols)
        if(APPLE)
            find_program(DSYMUTIL_EXECUTABLE dsymutil)
            if(DSYMUTIL_EXECUTABLE)
                add_custom_command(
                    TARGET ${PROJECT_NAME}
                    POST_BUILD
                    COMMAND "${DSYMUTIL_EXECUTABLE}" "$<TARGET_FILE:${PROJECT_NAME}>" -o "$<TARGET_FILE:${PROJECT_NAME}>.dSYM"
                    COMMENT "Generating dSYM for ${PROJECT_NAME}"
                    VERBATIM
                )
                install(DIRECTORY "$<TARGET_FILE:${PROJECT_NAME}>.dSYM"
                    DESTINATION ${CMAKE_INSTALL_BINDIR}
                    CONFIGURATIONS Debug RelWithDebInfo
                    OPTIONAL
                )
            endif()
        elseif(UNIX)
            if(CMAKE_OBJCOPY)
                add_custom_command(
                    TARGET ${PROJECT_NAME}
                    POST_BUILD
                    COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:${PROJECT_NAME}>" "$<TARGET_FILE:${PROJECT_NAME}>.debug"
                    COMMENT "Generating debug symbols for ${PROJECT_NAME}"
                    VERBATIM
                )
                install(FILES "$<TARGET_FILE:${PROJECT_NAME}>.debug"
                    DESTINATION ${CMAKE_INSTALL_BINDIR}
                    CONFIGURATIONS Debug RelWithDebInfo
                    OPTIONAL
                )
            endif()
        endif()
    endif()
endif()

# make install target of resources directory
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/resources/"
)

# Generate version config
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    VERSION ${CMAKE_PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Generate config file
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

# Install all config files
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

# Export targets
install(
    EXPORT ${PROJECT_NAME}Targets
    NAMESPACE ${PROJECT_PREFIX}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

# build tests
if(DECENTRALISED_ART_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Generate current timestamp
string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)
file(WRITE "${CMAKE_BINARY_DIR}/build_timestamp" "${BUILD_TIMESTAMP}")
install(FILES "${CMAKE_BINARY_DIR}/build_timestamp"
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
