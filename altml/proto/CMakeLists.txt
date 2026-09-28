set(ALTML_PROTO_FILES
    common.proto
    feature.proto
    prediction.proto
    system.proto)

add_library(altml_proto STATIC)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
protobuf_generate(
    TARGET altml_proto
    LANGUAGE cpp
    PROTOS ${ALTML_PROTO_FILES}
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(altml_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(altml_proto PUBLIC protobuf::libprotobuf)

set(ALTTECH_PROTO_FILES
    alttech/common.proto
    alttech/feature.proto
    alttech/analytics.proto
    alttech/microstructure.proto
    alttech/system.proto)
add_library(altml_proto_alttech STATIC)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated_alttech")
protobuf_generate(
    TARGET altml_proto_alttech
    LANGUAGE cpp
    PROTOS ${ALTTECH_PROTO_FILES}
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_alttech")
target_include_directories(altml_proto_alttech
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated_alttech")
target_link_libraries(altml_proto_alttech PUBLIC protobuf::libprotobuf)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/altml_schema.desc"
    COMMAND protobuf::protoc
        --descriptor_set_out="${CMAKE_BINARY_DIR}/altml_schema.desc"
        --include_imports
        -I "${CMAKE_CURRENT_SOURCE_DIR}"
        ${ALTML_PROTO_FILES}
    DEPENDS ${ALTML_PROTO_FILES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_custom_target(altml_proto_descriptor ALL
    DEPENDS "${CMAKE_BINARY_DIR}/altml_schema.desc")

