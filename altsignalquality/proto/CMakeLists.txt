# altsignalquality.v1 (owned) + the vendored altstrategy.v1 decision contract.
#
# decision.proto is vendored VERBATIM from altstrategy/proto/decision.proto so
# the final signal can carry AltStrategy's proposal without restating it; the
# copy is byte-compared against the producer by tests/unit/test_wire_contract.cpp
# when the sibling checkout is present.
set(ALTSQ_PROTO_FILES
    signalquality.proto
    vendor/decision.proto)

add_library(altsignalquality_proto STATIC)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
protobuf_generate(
    TARGET altsignalquality_proto
    LANGUAGE cpp
    PROTOS ${ALTSQ_PROTO_FILES}
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(altsignalquality_proto
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(altsignalquality_proto PUBLIC protobuf::libprotobuf)

# Descriptor set shipped with releases (schema audit / tooling).
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/altsignalquality_schema.desc"
    COMMAND protobuf::protoc
        --descriptor_set_out="${CMAKE_BINARY_DIR}/altsignalquality_schema.desc"
        --include_imports
        -I "${CMAKE_CURRENT_SOURCE_DIR}"
        ${ALTSQ_PROTO_FILES}
    DEPENDS ${ALTSQ_PROTO_FILES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_custom_target(altsignalquality_proto_descriptor ALL
    DEPENDS "${CMAKE_BINARY_DIR}/altsignalquality_schema.desc")
