set(ALTTECH_PROTO_FILES
    common.proto
    trade.proto
    candle.proto
    feature.proto
    microstructure.proto
    structure.proto
    regime.proto
    status.proto
    marketdata.proto
    analytics.proto
    system.proto)

add_library(alttech_proto STATIC)
# CMake's protobuf_generate does not create a custom PROTOC_OUT_DIR on every
# supported distro version.  A clean build must not depend on a directory left
# behind by an earlier package script.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
protobuf_generate(
    TARGET alttech_proto
    LANGUAGE cpp
    PROTOS ${ALTTECH_PROTO_FILES}
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_include_directories(alttech_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(alttech_proto PUBLIC protobuf::libprotobuf)

# Descriptor set shipped with releases (schema audit / tooling).
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/alttech_schema.desc
    COMMAND protobuf::protoc
        --descriptor_set_out=${CMAKE_BINARY_DIR}/alttech_schema.desc
        --include_imports
        -I ${CMAKE_CURRENT_SOURCE_DIR}
        ${ALTTECH_PROTO_FILES}
    DEPENDS ${ALTTECH_PROTO_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_custom_target(alttech_proto_descriptor ALL DEPENDS ${CMAKE_BINARY_DIR}/alttech_schema.desc)
