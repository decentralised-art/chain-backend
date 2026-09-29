cmake_minimum_required(VERSION 3.30)

enable_testing()

include(GoogleTest)

set(UNIT_TEST_TARGET "DecentralisedArtServerTests")
set(STRESS_TEST_TARGET "DecentralisedArtServerStressTests")

project("${UNIT_TEST_TARGET}"   
    VERSION "0.0.1"
    DESCRIPTION "Decentralised Art Server tests"
    HOMEPAGE_URL "https://github.com/hypermusic-ai/dcn-server"
    LANGUAGES CXX
)

set(DECENTRALISED_ART_TEST_SOLC_PATH_DEFAULT "${SOLC_INSTALL_DIR}/bin/${SOLC_EXE_NAME}")
set(DECENTRALISED_ART_TEST_PT_PATH_DEFAULT "${PT_INSTALL_DIR}")

set(DECENTRALISED_ART_TEST_SOLC_PATH
    "${DECENTRALISED_ART_TEST_SOLC_PATH_DEFAULT}"
    CACHE FILEPATH "Path to the solc executable used by unit tests")
set(DECENTRALISED_ART_TEST_PT_PATH
    "${DECENTRALISED_ART_TEST_PT_PATH_DEFAULT}"
    CACHE PATH "Path to PT Solidity runtime directory used by unit tests")

function(configure_test_target test_target)
    target_include_directories("${test_target}"
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>")

    target_compile_definitions("${test_target}"
        PRIVATE
            DECENTRALISED_ART_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}"
            DECENTRALISED_ART_TEST_SOLC_PATH="${DECENTRALISED_ART_TEST_SOLC_PATH}"
            DECENTRALISED_ART_TEST_PT_PATH="${DECENTRALISED_ART_TEST_PT_PATH}")

    target_link_libraries("${test_target}"
        PRIVATE
            GTest::gtest
            spdlog::spdlog
            sqlite3
            DecentralisedArtServerLib)

    set_target_properties("${test_target}" PROPERTIES CXX_STANDARD 23)

    if(MSVC)
        target_compile_options("${test_target}" PRIVATE /bigobj)
    endif()

    if(APPLE)
        set_target_properties("${test_target}" PROPERTIES BUILD_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties("${test_target}" PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
endfunction()

function(copy_evmone_runtime_to_target test_target)
    if(WIN32)
        add_custom_command(TARGET "${test_target}" POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVMONE_SHARED_LIBRARY_PATH}"
                "$<TARGET_FILE_DIR:${test_target}>/${EVMONE_SHARED_LIBRARY}"
            VERBATIM)
    elseif(APPLE)
        add_custom_command(TARGET "${test_target}" POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVMONE_SHARED_LIBRARY_PATH}"
                "$<TARGET_FILE_DIR:${test_target}>/${EVMONE_SHARED_LIBRARY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVMONE_SHARED_LIBRARY_PATH}"
                "$<TARGET_FILE_DIR:${test_target}>/libevmone.${EVMONE_MAJOR_VERSION}.${EVMONE_MINOR_VERSION}.dylib"
            VERBATIM)
    elseif(UNIX)
        add_custom_command(TARGET "${test_target}" POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVMONE_SHARED_LIBRARY_PATH}"
                "$<TARGET_FILE_DIR:${test_target}>/${EVMONE_SHARED_LIBRARY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVMONE_SHARED_LIBRARY_PATH}"
                "$<TARGET_FILE_DIR:${test_target}>/libevmone.so.${EVMONE_MAJOR_VERSION}.${EVMONE_MINOR_VERSION}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVMONE_SHARED_LIBRARY_PATH}"
                "$<TARGET_FILE_DIR:${test_target}>/libevmone.so.${EVMONE_MAJOR_VERSION}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVMONE_SHARED_LIBRARY_PATH}"
                "$<TARGET_FILE_DIR:${test_target}>/libevmone.so"
            VERBATIM)
    endif()
endfunction()

add_executable("${UNIT_TEST_TARGET}"    
    "src/tests.cpp"

    # --- pt module test files ---
    "src/pt/transformation.cpp"
    "src/pt/condition.cpp"
    "src/pt/connector.cpp"
    "src/pt/bindings.cpp"
    "src/pt/execute.cpp"
    "src/pt/initial_deployment.cpp"
    "src/pt/deploy_entities.cpp"
    "src/pt/publication.cpp"
    "src/pt/artifact.cpp"
    "src/pt/format.cpp"
    "src/api_format.cpp"
    "src/api_formats.cpp"
    "src/api_account.cpp"
    "src/api_accounts.cpp"
    "src/url.cpp"
    "src/db_first_runtime.cpp"
    "src/pt/proxy_upgrade.cpp"
    "src/registry.cpp"
    "src/registry_store_schema.cpp"
    "src/api_publish.cpp"
    "src/chain_abi.cpp"
    "src/native_process.cpp"
    "src/evm_storage.cpp"

    # --- rpc module test files ---
    "src/rpc/http_transport_tests.cpp"

    # --- events module test files ---
    "src/events/decoder_tests.cpp"
    "src/events/hot_store_ingest_tests.cpp"
    "src/events/projection_outbox_tests.cpp"
    "src/events/finality_reorg_tests.cpp"
    "src/events/feed_stream_sse_tests.cpp"
    "src/events/feed_history_tests.cpp"
    "src/events/archive_export_prune_tests.cpp"
    "src/events/recovery_failpoint_tests.cpp"
    "src/events/concurrency_backpressure_tests.cpp"
    "src/events/maintenance_wal_integrity_tests.cpp"
    "src/events/source_adapter_tests.cpp"
    "src/events/rpc_source_tests.cpp"
    "src/events/registry_projection_tests.cpp"
    "src/events/registry_source_precedence_tests.cpp"
    "src/events/registry_e2e_tests.cpp"

    # --- publish module test files ---
    "src/publish/publish_tests.cpp"

    # --- feed module test files ---
    "src/feed/feed_store_tests.cpp"
    "src/feed/feed_facade_tests.cpp"
    "src/feed/feed_projector_tests.cpp"
    "src/feed/archive_shard_consolidation_tests.cpp"
)

configure_test_target("${UNIT_TEST_TARGET}")
copy_evmone_runtime_to_target("${UNIT_TEST_TARGET}")
gtest_discover_tests("${UNIT_TEST_TARGET}" PROPERTIES LABELS "unit")

if(DECENTRALISED_ART_BUILD_STRESS_TESTS)
    add_executable("${STRESS_TEST_TARGET}"
        "src/stress/tests.cpp"
        "src/stress/registry_stress.cpp"
        "src/stress/events_load.cpp")

    configure_test_target("${STRESS_TEST_TARGET}")
    copy_evmone_runtime_to_target("${STRESS_TEST_TARGET}")
    gtest_discover_tests("${STRESS_TEST_TARGET}" PROPERTIES LABELS "stress")
endif()
