# The versioned Protobuf contract. Package `altspotter.v1` (generated C++
# namespace `altspotter::v1`); market-data messages only.
set(ALTSPOTTER_PROTO_FILES
    common.proto
    trade.proto
    candle.proto
    instrument.proto
    marketdata.proto
    history.proto
    system.proto)

add_library(altspotter_proto STATIC)
# CMake's protobuf_generate does not create a custom PROTOC_OUT_DIR on every
# supported distro version.  A clean build must not depend on a directory left
# behind by an earlier package script.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
protobuf_generate(
    TARGET altspotter_proto
    LANGUAGE cpp
    PROTOS ${ALTSPOTTER_PROTO_FILES}
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_include_directories(altspotter_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(altspotter_proto PUBLIC protobuf::libprotobuf)

# Descriptor set shipped with releases (schema audit / tooling).
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/altspotter_schema.desc
    COMMAND protobuf::protoc
        --descriptor_set_out=${CMAKE_BINARY_DIR}/altspotter_schema.desc
        --include_imports
        -I ${CMAKE_CURRENT_SOURCE_DIR}
        ${ALTSPOTTER_PROTO_FILES}
    DEPENDS ${ALTSPOTTER_PROTO_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_custom_target(altspotter_proto_descriptor ALL DEPENDS ${CMAKE_BINARY_DIR}/altspotter_schema.desc)
