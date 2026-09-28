set(ALTSTRATEGY_PROTO_FILES
    # --- altstrategy.v1 adaptive-decision contract (AltStrategy-owned) ---
    decision.proto
    # --- vendored upstream contracts consumed by the context_assembler ---
    altml_forecast.proto
    alttech_analytics.proto)

add_library(altstrategy_proto STATIC)
# CMake's protobuf_generate does not create a custom PROTOC_OUT_DIR on every
# supported distro version.  A clean build must not depend on a directory left
# behind by an earlier package script.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
protobuf_generate(
    TARGET altstrategy_proto
    LANGUAGE cpp
    PROTOS ${ALTSTRATEGY_PROTO_FILES}
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_include_directories(altstrategy_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(altstrategy_proto PUBLIC protobuf::libprotobuf)

# Descriptor set shipped with releases (schema audit / tooling).
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/altstrategy_schema.desc
    COMMAND protobuf::protoc
        --descriptor_set_out=${CMAKE_BINARY_DIR}/altstrategy_schema.desc
        --include_imports
        -I ${CMAKE_CURRENT_SOURCE_DIR}
        ${ALTSTRATEGY_PROTO_FILES}
    DEPENDS ${ALTSTRATEGY_PROTO_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_custom_target(altstrategy_proto_descriptor ALL DEPENDS ${CMAKE_BINARY_DIR}/altstrategy_schema.desc)
