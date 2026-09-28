# altsignalgateway.v1 (owned) + the vendored upstream contracts.
#
# signalquality.proto and decision.proto are vendored VERBATIM from
# altsignalquality/ and altstrategy/ so the canonical FINAL signal can reuse the
# PRODUCERS' OWN enums instead of restating them. The copies are byte-compared
# against the producers by tests/unit/test_wire_contract.cpp when the sibling
# checkouts are present — a drifted copy would let this service map one enum
# value onto another in silence, and only for the values that moved.
#
# The import root is proto/, which is why signalquality.proto's own
# `import "vendor/decision.proto"` resolves without editing the vendored file.
set(ASGW_PROTO_FILES
    gateway.proto
    vendor/signalquality.proto
    vendor/decision.proto)

add_library(altsignalgateway_proto STATIC)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
protobuf_generate(
    TARGET altsignalgateway_proto
    LANGUAGE cpp
    PROTOS ${ASGW_PROTO_FILES}
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(altsignalgateway_proto
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(altsignalgateway_proto PUBLIC protobuf::libprotobuf)

# Descriptor set shipped with releases: an external consumer can generate its
# own bindings from this without a copy of the .proto tree.
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/altsignalgateway_schema.desc"
    COMMAND protobuf::protoc
        --descriptor_set_out="${CMAKE_BINARY_DIR}/altsignalgateway_schema.desc"
        --include_imports
        -I "${CMAKE_CURRENT_SOURCE_DIR}"
        ${ASGW_PROTO_FILES}
    DEPENDS ${ASGW_PROTO_FILES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_custom_target(altsignalgateway_proto_descriptor ALL
    DEPENDS "${CMAKE_BINARY_DIR}/altsignalgateway_schema.desc")
