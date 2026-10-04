function(protobuf_generate)
    set(options)
    set(oneValueArgs NAME PROTO_SRC_DIR PROTO_DST_DIR)
    set(multiValueArgs)
    cmake_parse_arguments(protobuf_generate "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )

    if(NOT Protobuf_PROTOC_EXECUTABLE)
            message(FATAL_ERROR "Protobuf not found. Please install protobuf or set Protobuf_PROTOC_EXECUTABLE.")
    endif()

    message(STATUS "Generating proto files of target `${protobuf_generate_NAME}`\n\tfrom:\t${protobuf_generate_PROTO_SRC_DIR}\n\tto:\t${protobuf_generate_PROTO_DST_DIR}")

    file(GLOB_RECURSE PROTO_FILES "${protobuf_generate_PROTO_SRC_DIR}/*.proto")

    make_directory(${protobuf_generate_PROTO_DST_DIR})

    set(PROTO_GEN_SRCS "")
    set(PROTO_GEN_HDRS "")
    foreach(PROTO_FILE IN LISTS PROTO_FILES)
        
        file(RELATIVE_PATH REL_PATH ${protobuf_generate_PROTO_SRC_DIR} ${PROTO_FILE})
        get_filename_component(PROTO_NAME ${REL_PATH} NAME_WE)

        set(GEN_SRC "${protobuf_generate_PROTO_DST_DIR}/${PROTO_NAME}.pb.cc")
        set(GEN_HDR "${protobuf_generate_PROTO_DST_DIR}/${PROTO_NAME}.pb.h")
    
        list(APPEND PROTO_GEN_SRCS ${GEN_SRC})
        list(APPEND PROTO_GEN_HDRS ${GEN_HDR})
    
        add_custom_command(
            OUTPUT ${GEN_SRC} ${GEN_HDR}
            COMMAND ${Protobuf_PROTOC_EXECUTABLE} --proto_path=${protobuf_generate_PROTO_SRC_DIR} --cpp_out=${protobuf_generate_PROTO_DST_DIR} "${REL_PATH}"
            DEPENDS "${PROTO_FILE}"
            WORKING_DIRECTORY ${protobuf_generate_PROTO_SRC_DIR}
            COMMENT "Generating C++ files from ${PROTO_FILE}"
            VERBATIM
        )    
    endforeach()

    message(STATUS "Generated proto files: ${PROTO_GEN_SRCS} ${PROTO_GEN_HDRS}")

    # create target from generated files
    add_library(${protobuf_generate_NAME} OBJECT ${PROTO_GEN_SRCS} ${PROTO_GEN_HDRS})

    # Generated code is third-party: no warnings from building it or including its headers.
    silence_warnings(TARGETS ${protobuf_generate_NAME})
    target_include_directories(${protobuf_generate_NAME} SYSTEM PUBLIC 
        "$<BUILD_INTERFACE:${protobuf_generate_PROTO_DST_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/generated>"
    )
    
    # Add dependency with protobuf
    target_link_libraries(${protobuf_generate_NAME}    
    PUBLIC
        protobuf::libprotobuf
    )

    add_dependencies(${protobuf_generate_NAME} protobuf::protoc)

    # create install target for generated files
    install(DIRECTORY ${protobuf_generate_PROTO_DST_DIR}/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/generated
        FILES_MATCHING PATTERN "*.h*"  # Only install .h* files
    )

endfunction(protobuf_generate)