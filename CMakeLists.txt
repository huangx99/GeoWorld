cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(GeoWorld VERSION 0.1.0 LANGUAGES CXX)

option(GW_BUILD_APPS "Build GeoWorld executables" ON)
option(GW_BUILD_TESTS "Build GeoWorld tests" ON)
option(GW_BUILD_BENCHMARKS "Build GeoWorld benchmarks" ON)
option(GW_REQUIRE_FLECS "Fail configuration when flecs is unavailable" OFF)
option(GW_BUILD_TOOLING_GUI "Build the Qt behavior authoring application" OFF)
option(GW_BUILD_M4 "Build M4 network targets (protocol, gateway)" OFF)
option(GW_BUILD_M5 "Build M5 persistence targets (WAL, durable log)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CTest)

if(GW_BUILD_TOOLING_GUI)
    include(FetchContent)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCS OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(QT_NODES_DEVELOPER_DEFAULTS OFF CACHE BOOL "" FORCE)
    set(USE_QT6 ON CACHE BOOL "" FORCE)
    FetchContent_Declare(qt_nodes
        GIT_REPOSITORY https://github.com/paceholder/nodeeditor.git
        GIT_TAG 7c6341a66a8e46b8988140b9e60d892b6a3560b3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(qt_nodes)
    set(BUILD_TESTING "${GW_BUILD_TESTS}")
endif()

add_library(geoworld_foundation STATIC
    libs/foundation/src/random.cpp
    libs/foundation/src/thread_pool.cpp
    libs/foundation/src/version.cpp
)
add_library(geoworld::foundation ALIAS geoworld_foundation)
find_package(Threads REQUIRED)
target_link_libraries(geoworld_foundation PUBLIC Threads::Threads)
target_include_directories(geoworld_foundation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/foundation/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_library(geoworld_schema STATIC
    libs/schema/src/migration.cpp
    libs/schema/src/schema_registry.cpp
)
add_library(geoworld::schema ALIAS geoworld_schema)
target_link_libraries(geoworld_schema PUBLIC geoworld::foundation)
target_include_directories(geoworld_schema PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/schema/include>
)

add_library(geoworld_world_model STATIC
    libs/world_model/src/world.cpp
)
add_library(geoworld::world_model ALIAS geoworld_world_model)
target_link_libraries(geoworld_world_model PUBLIC geoworld::foundation geoworld::schema)
target_include_directories(geoworld_world_model PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/world_model/include>
)

find_package(flecs CONFIG QUIET)
find_package(EnTT CONFIG QUIET)
find_package(flatbuffers CONFIG QUIET)
find_package(nlohmann_json CONFIG QUIET)
find_package(nlohmann_json_schema_validator CONFIG QUIET)
find_package(graphviz CONFIG QUIET)
find_package(Boost 1.91.0 EXACT REQUIRED)
find_package(PROJ CONFIG QUIET)
find_package(s2 CONFIG QUIET)
find_package(behaviortree_cpp CONFIG QUIET)
find_package(sol2 CONFIG QUIET)
find_package(Lua CONFIG QUIET)
find_package(unofficial-lua CONFIG QUIET)
find_package(fmt CONFIG QUIET)
find_package(spdlog CONFIG QUIET)
find_path(WASMEDGE_INCLUDE_DIR wasmedge/wasmedge.h)
find_library(WASMEDGE_LIBRARY NAMES wasmedge)
if(flecs_FOUND)
    add_library(geoworld_ecs_runtime STATIC
        libs/ecs_runtime/src/schema.cpp
        libs/ecs_runtime/src/runtime.cpp
    )
    add_library(geoworld::ecs_runtime ALIAS geoworld_ecs_runtime)
    target_link_libraries(geoworld_ecs_runtime
        PUBLIC geoworld::foundation geoworld::world_model
        PRIVATE $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>
    )
    target_include_directories(geoworld_ecs_runtime PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/ecs_runtime/include>
    )

    if(flatbuffers_FOUND)
        set(GW_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
        set(GW_RUNTIME_SNAPSHOT_HEADER "${GW_GENERATED_DIR}/runtime_snapshot_generated.h")
        add_custom_command(
            OUTPUT "${GW_RUNTIME_SNAPSHOT_HEADER}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${GW_GENERATED_DIR}"
            COMMAND $<TARGET_FILE:flatbuffers::flatc>
                    --cpp --scoped-enums -o "${GW_GENERATED_DIR}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/data/runtime_snapshot.fbs"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schemas/data/runtime_snapshot.fbs"
            VERBATIM
        )
        target_sources(geoworld_ecs_runtime PRIVATE
            libs/ecs_runtime/src/snapshot_codec.cpp
            "${GW_RUNTIME_SNAPSHOT_HEADER}"
        )
        target_include_directories(geoworld_ecs_runtime PRIVATE "${GW_GENERATED_DIR}")
        target_link_libraries(geoworld_ecs_runtime PRIVATE flatbuffers::flatbuffers)
    endif()

elseif(GW_REQUIRE_FLECS)
    message(FATAL_ERROR "GW_REQUIRE_FLECS is ON but flecs was not found")
else()
    message(STATUS "flecs not found; ecs_runtime target is disabled")
endif()

add_library(geoworld_simulation STATIC
    libs/simulation/src/command_buffer.cpp
    libs/simulation/src/tick.cpp
)
add_library(geoworld::simulation ALIAS geoworld_simulation)
target_link_libraries(geoworld_simulation PUBLIC geoworld::foundation geoworld::world_model)
target_include_directories(geoworld_simulation PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/simulation/include>
)

add_library(geoworld_rules STATIC
    libs/rules/src/event_bus.cpp
    libs/rules/src/expression.cpp
    libs/rules/src/rule_engine.cpp
    libs/rules/src/plugin_registry.cpp
)
add_library(geoworld::rules ALIAS geoworld_rules)
target_link_libraries(geoworld_rules PUBLIC
    geoworld::foundation
    geoworld::schema
    geoworld::simulation
    geoworld::world_model
    ${CMAKE_DL_LIBS}
)
target_include_directories(geoworld_rules PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/rules/include>
)

if(GW_BUILD_TESTS OR GW_BUILD_BENCHMARKS)
    add_library(geoworld-m3-rule-plugin SHARED tests/fixtures/m3_rule_plugin.cpp)
    target_include_directories(geoworld-m3-rule-plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/rules/include
    )
    target_compile_definitions(geoworld-m3-rule-plugin PRIVATE
        GEOWORLD_RULE_PLUGIN_BUILD=1
    )
endif()

add_library(geoworld_ai STATIC
    libs/ai/src/fsm.cpp
    libs/ai/src/hfsm.cpp
    libs/ai/src/task_scheduler.cpp
    libs/ai/src/decision.cpp
    libs/ai/src/bt_runtime.cpp
    libs/ai/src/lua_runtime.cpp
    libs/ai/src/wasm_runtime.cpp
)
add_library(geoworld::ai ALIAS geoworld_ai)
target_link_libraries(geoworld_ai PUBLIC
    geoworld::foundation
    geoworld::simulation
    geoworld::world_model
)
target_include_directories(geoworld_ai PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/ai/include>
)
if(TARGET BT::behaviortree_cpp)
    target_link_libraries(geoworld_ai PRIVATE BT::behaviortree_cpp)
    target_compile_definitions(geoworld_ai PUBLIC GW_HAS_BEHAVIORTREE_CPP=1)
else()
    target_compile_definitions(geoworld_ai PUBLIC GW_HAS_BEHAVIORTREE_CPP=0)
endif()
if(TARGET sol2::sol2 AND TARGET unofficial::lua::lua)
    target_link_libraries(geoworld_ai PRIVATE sol2::sol2 unofficial::lua::lua)
    target_compile_definitions(geoworld_ai PUBLIC GW_HAS_SOL2=1)
else()
    target_compile_definitions(geoworld_ai PUBLIC GW_HAS_SOL2=0)
endif()
if(WASMEDGE_INCLUDE_DIR AND WASMEDGE_LIBRARY)
    target_include_directories(geoworld_ai PRIVATE ${WASMEDGE_INCLUDE_DIR})
    target_link_libraries(geoworld_ai PRIVATE ${WASMEDGE_LIBRARY})
    if(TARGET spdlog::spdlog)
        target_link_libraries(geoworld_ai PRIVATE spdlog::spdlog)
    endif()
    if(TARGET fmt::fmt)
        target_link_libraries(geoworld_ai PRIVATE fmt::fmt)
    endif()
    target_compile_definitions(geoworld_ai PUBLIC GW_HAS_WASMEDGE=1)
else()
    target_compile_definitions(geoworld_ai PUBLIC GW_HAS_WASMEDGE=0)
endif()

add_library(geoworld_observability STATIC
    libs/observability/src/log_buffer.cpp
    libs/observability/src/logger.cpp
    libs/observability/src/metrics.cpp
)
add_library(geoworld::observability ALIAS geoworld_observability)
target_link_libraries(geoworld_observability PUBLIC geoworld::foundation)
target_include_directories(geoworld_observability PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/observability/include>
)

add_library(geoworld_debug STATIC
    libs/debug/src/state_hash.cpp
    libs/debug/src/tick_controller.cpp
    libs/debug/src/world_inspector.cpp
)
add_library(geoworld::debug ALIAS geoworld_debug)
target_link_libraries(geoworld_debug PUBLIC geoworld::foundation geoworld::world_model)
target_include_directories(geoworld_debug PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/debug/include>
)

set(GW_TOOLING_SOURCES libs/tooling/src/artifact.cpp)
if(flatbuffers_FOUND AND nlohmann_json_FOUND AND nlohmann_json_schema_validator_FOUND)
    set(GW_GENERATED_TOOLING_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/tooling")
    set(GW_BEHAVIOR_ARTIFACT_HEADER "${GW_GENERATED_TOOLING_DIR}/behavior_artifact_generated.h")
    add_custom_command(
        OUTPUT "${GW_BEHAVIOR_ARTIFACT_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GW_GENERATED_TOOLING_DIR}"
        COMMAND $<TARGET_FILE:flatbuffers::flatc>
                --cpp --scoped-enums -o "${GW_GENERATED_TOOLING_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/tooling/behavior_artifact.fbs"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schemas/tooling/behavior_artifact.fbs"
        VERBATIM
    )
    list(APPEND GW_TOOLING_SOURCES
        libs/tooling/src/behavior_document.cpp
        libs/tooling/src/graphviz_layout.cpp
        "${GW_BEHAVIOR_ARTIFACT_HEADER}"
    )
endif()

add_library(geoworld_tooling STATIC ${GW_TOOLING_SOURCES})
add_library(geoworld::tooling ALIAS geoworld_tooling)
target_link_libraries(geoworld_tooling PUBLIC geoworld::foundation geoworld::schema)
target_include_directories(geoworld_tooling PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/tooling/include>
)
if(flatbuffers_FOUND AND nlohmann_json_FOUND AND nlohmann_json_schema_validator_FOUND)
    target_include_directories(geoworld_tooling PRIVATE "${GW_GENERATED_TOOLING_DIR}")
    target_link_libraries(geoworld_tooling PRIVATE
        flatbuffers::flatbuffers
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
    )
    target_compile_definitions(geoworld_tooling PUBLIC GW_HAS_BEHAVIOR_TOOLING=1)
    if(graphviz_FOUND)
        target_link_libraries(geoworld_tooling PRIVATE
            $<LINK_GROUP:RESCAN,
                graphviz::gvc,
                $<IF:$<CONFIG:Debug>,
                    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib/libgvplugin_dot_layout.a,
                    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libgvplugin_dot_layout.a>,
                $<IF:$<CONFIG:Debug>,
                    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib/libdotgen.a,
                    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libdotgen.a>>)
        target_compile_definitions(geoworld_tooling PUBLIC GW_HAS_GRAPHVIZ=1)
    else()
        target_compile_definitions(geoworld_tooling PUBLIC GW_HAS_GRAPHVIZ=0)
    endif()
else()
    target_compile_definitions(geoworld_tooling PUBLIC GW_HAS_BEHAVIOR_TOOLING=0 GW_HAS_GRAPHVIZ=0)
endif()

target_sources(geoworld_ai PRIVATE libs/ai/src/artifact_runtime.cpp)
target_link_libraries(geoworld_ai PUBLIC geoworld::tooling)

add_library(geoworld_spatial STATIC
    libs/spatial/src/aoi.cpp
    libs/spatial/src/cell_grid.cpp
    libs/spatial/src/coordinates.cpp
    libs/spatial/src/s2_key.cpp
    libs/spatial/src/cell_lifecycle.cpp
    libs/spatial/src/spatial_query.cpp
    libs/spatial/src/streaming.cpp
)
add_library(geoworld::spatial ALIAS geoworld_spatial)
target_link_libraries(geoworld_spatial PUBLIC geoworld::foundation)
if(TARGET Boost::headers)
    target_link_libraries(geoworld_spatial PUBLIC Boost::headers)
elseif(TARGET Boost::boost)
    target_link_libraries(geoworld_spatial PUBLIC Boost::boost)
endif()
target_include_directories(geoworld_spatial PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/spatial/include>
)
if(s2_FOUND)
    target_link_libraries(geoworld_spatial PRIVATE s2::s2)
    target_compile_definitions(geoworld_spatial PUBLIC GW_HAS_S2=1)
else()
    target_compile_definitions(geoworld_spatial PUBLIC GW_HAS_S2=0)
endif()

if(TARGET geoworld_ecs_runtime)
    add_library(geoworld_world_runtime STATIC
        runtime/world_runtime/src/world_runtime.cpp
    )
    add_library(geoworld::world_runtime ALIAS geoworld_world_runtime)
    target_link_libraries(geoworld_world_runtime PUBLIC
        geoworld::ai
        geoworld::debug
        geoworld::ecs_runtime
        geoworld::observability
        geoworld::schema
        geoworld::simulation
        geoworld::spatial
        geoworld::world_model
    )
    target_include_directories(geoworld_world_runtime PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/world_runtime/include>
    )
endif()

add_library(geoworld_projection STATIC
    libs/projection/src/canonical.cpp
    libs/projection/src/config.cpp
    libs/projection/src/connection.cpp
    libs/projection/src/engine.cpp
    libs/projection/src/policy.cpp
)
add_library(geoworld::projection ALIAS geoworld_projection)
target_link_libraries(geoworld_projection PUBLIC
    geoworld::foundation
    geoworld::schema
    geoworld::spatial
    geoworld::world_model
)
target_include_directories(geoworld_projection PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/projection/include>
)

# M5-A：持久化 WAL 与 durable 日志。目标由 GW_BUILD_M5 显式控制。
if(GW_BUILD_M5)
    if(NOT flatbuffers_FOUND)
        message(FATAL_ERROR "GW_BUILD_M5 requires flatbuffers")
    endif()
    find_package(Crc32c CONFIG REQUIRED)
    find_package(zstd CONFIG REQUIRED)
    find_package(mio CONFIG REQUIRED)
    if(NOT TARGET Boost::headers AND NOT TARGET Boost::boost)
        message(FATAL_ERROR "GW_BUILD_M5 requires Boost headers (boost-uuid)")
    endif()

    set(GW_M5_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/m5")
    set(GW_M5_PERSISTENCE_GENERATED
        "${GW_M5_GENERATED_DIR}/wal_record_generated.h"
        "${GW_M5_GENERATED_DIR}/checkpoint_manifest_generated.h"
        "${GW_M5_GENERATED_DIR}/replay_manifest_generated.h"
        "${GW_M5_GENERATED_DIR}/branch_manifest_generated.h"
        "${GW_M5_GENERATED_DIR}/world_checkpoint_generated.h"
    )
    add_custom_command(
        OUTPUT ${GW_M5_PERSISTENCE_GENERATED}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GW_M5_GENERATED_DIR}"
        COMMAND $<TARGET_FILE:flatbuffers::flatc>
                --cpp --scoped-enums -o "${GW_M5_GENERATED_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/wal_record.fbs"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/checkpoint_manifest.fbs"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/replay_manifest.fbs"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/branch_manifest.fbs"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/world_checkpoint.fbs"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/wal_record.fbs"
            "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/checkpoint_manifest.fbs"
            "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/replay_manifest.fbs"
            "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/branch_manifest.fbs"
            "${CMAKE_CURRENT_SOURCE_DIR}/schemas/persistence/world_checkpoint.fbs"
        VERBATIM
    )

    add_library(geoworld_persistence STATIC
        libs/persistence/src/manifest.cpp
        libs/persistence/src/storage.cpp
        libs/persistence/src/types.cpp
        libs/persistence/src/wal_codec.cpp
        libs/persistence/src/wal_reader.cpp
        libs/persistence/src/wal_writer.cpp
        # 仅头文件：建立对 flatc codegen 自定义命令的依赖；生成类型不进公共 API。
        ${GW_M5_PERSISTENCE_GENERATED}
    )
    add_library(geoworld::persistence ALIAS geoworld_persistence)
    # CRC32C/zstd/mio/Boost/FlatBuffers 只进入实现层，全部为 PRIVATE。
    target_link_libraries(geoworld_persistence
        PUBLIC geoworld::foundation
        PRIVATE
            flatbuffers::flatbuffers
            Crc32c::crc32c
            zstd::libzstd
            mio::mio
    )
    if(TARGET Boost::headers)
        target_link_libraries(geoworld_persistence PRIVATE Boost::headers)
    else()
        target_link_libraries(geoworld_persistence PRIVATE Boost::boost)
    endif()
    target_include_directories(geoworld_persistence PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/persistence/include>
    )
    target_include_directories(geoworld_persistence PRIVATE
        "${GW_M5_GENERATED_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/persistence/src"
    )
    target_compile_options(geoworld_persistence PRIVATE -Wall -Wextra)
endif()

# M4-B/M4-C：版本化协议与 Gateway。网络目标由 GW_BUILD_M4 显式控制。
if(GW_BUILD_M4)
    if(NOT flatbuffers_FOUND)
        message(FATAL_ERROR "GW_BUILD_M4 requires flatbuffers")
    endif()
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)

    set(GW_M4_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/m4")
    set(GW_M4_STREAM_GENERATED
        "${GW_M4_GENERATED_DIR}/world_stream_generated.h"
        "${GW_M4_GENERATED_DIR}/client_stream_generated.h"
    )
    add_custom_command(
        OUTPUT ${GW_M4_STREAM_GENERATED}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GW_M4_GENERATED_DIR}"
        COMMAND $<TARGET_FILE:flatbuffers::flatc>
                --cpp --scoped-enums -o "${GW_M4_GENERATED_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/data/world_stream.fbs"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/data/client_stream.fbs"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/schemas/data/world_stream.fbs"
            "${CMAKE_CURRENT_SOURCE_DIR}/schemas/data/client_stream.fbs"
        VERBATIM
    )

    set(GW_M4_CONTROL_GENERATED
        "${GW_M4_GENERATED_DIR}/geoworld_control.pb.cc"
        "${GW_M4_GENERATED_DIR}/geoworld_control.pb.h"
        "${GW_M4_GENERATED_DIR}/geoworld_control.grpc.pb.cc"
        "${GW_M4_GENERATED_DIR}/geoworld_control.grpc.pb.h"
    )
    add_custom_command(
        OUTPUT ${GW_M4_CONTROL_GENERATED}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GW_M4_GENERATED_DIR}"
        COMMAND $<TARGET_FILE:protobuf::protoc>
                --cpp_out=${GW_M4_GENERATED_DIR}
                --grpc_out=${GW_M4_GENERATED_DIR}
                --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
                -I "${CMAKE_CURRENT_SOURCE_DIR}/schemas/control"
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/control/geoworld_control.proto"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schemas/control/geoworld_control.proto"
        VERBATIM
    )

    add_library(geoworld_protocol STATIC
        libs/protocol/src/codec.cpp
        libs/protocol/src/replica.cpp
        ${GW_M4_STREAM_GENERATED}
        ${GW_M4_CONTROL_GENERATED}
    )
    add_library(geoworld::protocol ALIAS geoworld_protocol)
    target_link_libraries(geoworld_protocol
        PUBLIC
            geoworld::foundation
            geoworld::schema
        PRIVATE
            flatbuffers::flatbuffers
            protobuf::libprotobuf
            gRPC::grpc++
    )
    target_include_directories(geoworld_protocol PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/protocol/include>
    )
    target_include_directories(geoworld_protocol PRIVATE "${GW_M4_GENERATED_DIR}")

    add_library(geoworld_gateway STATIC
        libs/gateway/src/auth.cpp
        libs/gateway/src/config.cpp
        libs/gateway/src/control_server.cpp
        libs/gateway/src/durable.cpp
        libs/gateway/src/errors.cpp
        libs/gateway/src/frame_codec.cpp
        libs/gateway/src/gateway_core.cpp
        libs/gateway/src/ownership.cpp
        libs/gateway/src/queues.cpp
        libs/gateway/src/session.cpp
        libs/gateway/src/stream_transport.cpp
        # 仅头文件：建立对 codegen 自定义命令的依赖，不重复编译 pb 源。
        "${GW_M4_GENERATED_DIR}/geoworld_control.pb.h"
        "${GW_M4_GENERATED_DIR}/geoworld_control.grpc.pb.h"
    )
    add_library(geoworld::gateway ALIAS geoworld_gateway)
    target_link_libraries(geoworld_gateway PUBLIC
        geoworld::projection
        geoworld::protocol
        geoworld::simulation
    )
    # 网络与生成代码只进入实现：gRPC/Protobuf/OpenSSL/Boost 均为 PRIVATE。
    target_link_libraries(geoworld_gateway PRIVATE
        flatbuffers::flatbuffers
        gRPC::grpc++
        protobuf::libprotobuf
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    if(TARGET Boost::headers)
        target_link_libraries(geoworld_gateway PRIVATE Boost::headers)
    elseif(TARGET Boost::boost)
        target_link_libraries(geoworld_gateway PRIVATE Boost::boost)
    endif()
    target_include_directories(geoworld_gateway PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/gateway/include>
    )
    target_include_directories(geoworld_gateway PRIVATE "${GW_M4_GENERATED_DIR}")

    # M5 durable admission 桥接：唯一暴露 persistence 类型的 gateway 源文件，
    # 只在 GW_BUILD_M5 下编译并链接（PUBLIC：组合根经桥接头接触 persistence 类型）。
    if(GW_BUILD_M5 AND TARGET geoworld_persistence)
        target_sources(geoworld_gateway PRIVATE libs/gateway/src/durable_persistence.cpp)
        target_link_libraries(geoworld_gateway PUBLIC geoworld::persistence)
        target_compile_definitions(geoworld_gateway PUBLIC GW_GATEWAY_DURABLE_PERSISTENCE=1)
    endif()

    # 参考客户端共享实现：gw-stream-client 与 m4 集成测试复用。
    add_library(geoworld_stream_client STATIC
        tools/gw-stream-client/stream_client.cpp
        tools/gw-stream-client/thin_client.cpp
        "${GW_M4_GENERATED_DIR}/geoworld_control.pb.h"
        "${GW_M4_GENERATED_DIR}/geoworld_control.grpc.pb.h"
    )
    target_include_directories(geoworld_stream_client PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/gw-stream-client>
    )
    target_include_directories(geoworld_stream_client PRIVATE "${GW_M4_GENERATED_DIR}")
    target_link_libraries(geoworld_stream_client PUBLIC
        geoworld::gateway
        geoworld::protocol
    )
    target_link_libraries(geoworld_stream_client PRIVATE
        gRPC::grpc++
        protobuf::libprotobuf
        flatbuffers::flatbuffers
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    if(TARGET Boost::headers)
        target_link_libraries(geoworld_stream_client PRIVATE Boost::headers)
    elseif(TARGET Boost::boost)
        target_link_libraries(geoworld_stream_client PRIVATE Boost::boost)
    endif()
endif()

if(GW_BUILD_APPS)
    add_executable(geoworldd apps/geoworldd/main.cpp)
    target_link_libraries(geoworldd PRIVATE
        geoworld::debug
        geoworld::observability
        geoworld::simulation
    )
    if(TARGET geoworld_ecs_runtime)
        target_link_libraries(geoworldd PRIVATE geoworld::ecs_runtime)
        target_compile_definitions(geoworldd PRIVATE GW_HAS_ECS_RUNTIME=1)
    endif()
    # M4 组合根需要 gateway 与 world_runtime 同时具备；缺一时保持原样可构建。
    if(TARGET geoworld_gateway AND TARGET geoworld_world_runtime)
        target_link_libraries(geoworldd PRIVATE
            geoworld::gateway
            geoworld::projection
            geoworld::world_runtime
        )
        target_compile_definitions(geoworldd PRIVATE GW_BUILD_M4=1)
    else()
        target_compile_definitions(geoworldd PRIVATE GW_BUILD_M4=0)
    endif()
    # M5 durable admission 接线需要 M4 组合根与 persistence 同时具备。
    if(GW_BUILD_M4 AND TARGET geoworld_persistence AND TARGET geoworld_world_runtime)
        target_link_libraries(geoworldd PRIVATE geoworld::persistence)
        target_compile_definitions(geoworldd PRIVATE GW_BUILD_M5=1)
    else()
        target_compile_definitions(geoworldd PRIVATE GW_BUILD_M5=0)
    endif()
    if(TARGET geoworld_stream_client)
        add_executable(gw-stream-client tools/gw-stream-client/main.cpp)
        target_link_libraries(gw-stream-client PRIVATE geoworld_stream_client)
    endif()

    add_executable(geoworld-replay apps/geoworld-replay/main.cpp)
    target_link_libraries(geoworld-replay PRIVATE geoworld::simulation)

    add_executable(geoworld-cli apps/geoworld-cli/main.cpp)
    target_link_libraries(geoworld-cli PRIVATE geoworld::world_model)

    add_executable(geoworld-showcase apps/geoworld-showcase/main.cpp)
    target_link_libraries(geoworld-showcase PRIVATE
        geoworld::ai
        geoworld::debug
        geoworld::rules
        geoworld::spatial
    )

    add_executable(gw-rule tools/gw-artifact/main.cpp)
    target_compile_definitions(gw-rule PRIVATE GW_ARTIFACT_KIND_RULE=1 GW_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(gw-rule PRIVATE geoworld::tooling)

    add_executable(gw-bt tools/gw-artifact/main.cpp)
    target_compile_definitions(gw-bt PRIVATE GW_ARTIFACT_KIND_BT=1 GW_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(gw-bt PRIVATE geoworld::tooling)

    add_executable(gw-st tools/gw-artifact/main.cpp)
    target_compile_definitions(gw-st PRIVATE GW_ARTIFACT_KIND_ST=1 GW_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(gw-st PRIVATE geoworld::tooling)
endif()

if(GW_BUILD_BENCHMARKS)
    add_executable(geoworld-m1-benchmark benchmarks/m1_core_benchmark.cpp)
    target_link_libraries(geoworld-m1-benchmark PRIVATE geoworld::debug geoworld::simulation)
    add_executable(geoworld-m3-benchmark benchmarks/m3_runtime_benchmark.cpp)
    target_link_libraries(geoworld-m3-benchmark PRIVATE geoworld::ai geoworld::rules)
    if(TARGET geoworld-m3-rule-plugin)
        add_dependencies(geoworld-m3-benchmark geoworld-m3-rule-plugin)
        target_compile_definitions(geoworld-m3-benchmark PRIVATE
            GW_BENCHMARK_RULE_PLUGIN_PATH="$<TARGET_FILE:geoworld-m3-rule-plugin>")
    endif()
    if(TARGET geoworld_ecs_runtime AND TARGET EnTT::EnTT)
        add_executable(geoworld-m1-ecs-comparison benchmarks/m1_ecs_comparison.cpp)
        target_link_libraries(geoworld-m1-ecs-comparison PRIVATE
            geoworld::ecs_runtime
            EnTT::EnTT
            $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>
        )
    endif()
    # R4 Gateway 扇出基准：只在 M4 目标可用时注册；与 m1/m3 一样不注册为 CTest 用例。
    if(TARGET geoworld_gateway AND TARGET geoworld_stream_client)
        # 依赖版本随基准结果输出（验收门 9）；取不到的标记 unknown，不硬编码。
        foreach(version_pair IN ITEMS
                "GW_BENCH_BOOST_VERSION:Boost_VERSION"
                "GW_BENCH_PROTOBUF_VERSION:Protobuf_VERSION"
                "GW_BENCH_GRPC_VERSION:gRPC_VERSION"
                "GW_BENCH_OPENSSL_VERSION:OPENSSL_VERSION_STR"
                "GW_BENCH_FLATBUFFERS_VERSION:flatbuffers_VERSION")
            string(REGEX REPLACE ":.*$" "" define_name "${version_pair}")
            string(REGEX REPLACE "^.*:" "" source_var "${version_pair}")
            if(DEFINED ${source_var} AND NOT "${${source_var}}" STREQUAL "")
                set(bench_dep_value "${${source_var}}")
            else()
                set(bench_dep_value "unknown")
            endif()
            list(APPEND bench_dep_definitions
                "${define_name}=\"${bench_dep_value}\"")
        endforeach()
        add_executable(geoworld-m4-benchmark benchmarks/m4_gateway_benchmark.cpp
            benchmarks/mi_new_override.cpp)
        target_link_libraries(geoworld-m4-benchmark PRIVATE
            geoworld::gateway
            geoworld::projection
            geoworld::protocol
            geoworld_stream_client
        )
        # mimalloc 仅为基准进程的分配器覆盖依赖：缺失时退回系统 malloc，
        # 基准语义与输出不受影响（仅性能差异）。
        find_package(mimalloc CONFIG QUIET)
        if(TARGET mimalloc-static)
            target_link_libraries(geoworld-m4-benchmark PRIVATE mimalloc-static)
            target_compile_definitions(geoworld-m4-benchmark PRIVATE GW_BENCH_MIMALLOC=1)
        endif()
        target_compile_definitions(geoworld-m4-benchmark PRIVATE
            GW_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
            ${bench_dep_definitions}
        )
        unset(bench_dep_definitions)
        unset(bench_dep_value)
    endif()
endif()

if(GW_BUILD_TOOLING_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL)
    find_package(Qt6QMinimalIntegrationPlugin CONFIG REQUIRED
        PATHS "${Qt6_DIR}/../Qt6Gui" NO_DEFAULT_PATH)
    set(CMAKE_AUTOMOC ON)
    add_executable(gw-behavior-editor
        tools/gw-behavior-editor/main.cpp
        tools/gw-behavior-editor/main_window.cpp
        tools/gw-behavior-editor/main_window.hpp
        tools/gw-behavior-editor/behavior_graph_model.cpp
        tools/gw-behavior-editor/behavior_graph_model.hpp
        tools/gw-behavior-editor/hfsm_graph_model.cpp
        tools/gw-behavior-editor/hfsm_graph_model.hpp
    )
    target_link_libraries(gw-behavior-editor PRIVATE
        geoworld::tooling
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::QMinimalIntegrationPlugin
        QtNodes::QtNodes
    )
    qt_add_resources(gw-behavior-editor behavior_schemas
        PREFIX "/schemas"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/tooling"
        FILES
            schemas/tooling/behavior_tree.schema.json
            schemas/tooling/hfsm.schema.json
    )
    if(GW_BUILD_TESTS)
        add_test(NAME geoworld.gw_bt.cli
            COMMAND ${CMAKE_COMMAND}
                -DTOOL=$<TARGET_FILE:gw-bt>
                -DDOCUMENT=${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures/m3_behavior_tree.json
                -DARTIFACT=${CMAKE_CURRENT_BINARY_DIR}/testing/m3_behavior_tree.gwbt
                -DLAYOUT=${CMAKE_CURRENT_BINARY_DIR}/testing/m3_behavior_tree.layout.json
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tooling_cli_test.cmake)
        add_test(NAME geoworld.gw_st.cli
            COMMAND ${CMAKE_COMMAND}
                -DTOOL=$<TARGET_FILE:gw-st>
                -DDOCUMENT=${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures/m3_hfsm.json
                -DARTIFACT=${CMAKE_CURRENT_BINARY_DIR}/testing/m3_hfsm.gwst
                -DLAYOUT=${CMAKE_CURRENT_BINARY_DIR}/testing/m3_hfsm.layout.json
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tooling_cli_test.cmake)
        add_test(NAME geoworld.behavior_editor.bt_smoke
            COMMAND gw-behavior-editor --smoke
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures/m3_behavior_tree.json")
        set_tests_properties(geoworld.behavior_editor.bt_smoke PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=minimal")
        add_test(NAME geoworld.behavior_editor.hfsm_smoke
            COMMAND gw-behavior-editor --smoke
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fixtures/m3_hfsm.json")
        set_tests_properties(geoworld.behavior_editor.hfsm_smoke PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=minimal")
    endif()
endif()

if(GW_BUILD_TESTS)
    add_executable(geoworld-foundation-test tests/unit/foundation_test.cpp)
    target_link_libraries(geoworld-foundation-test PRIVATE geoworld::foundation)
    add_test(NAME geoworld.foundation COMMAND geoworld-foundation-test)

    add_executable(geoworld-simulation-test tests/unit/simulation_test.cpp)
    target_link_libraries(geoworld-simulation-test PRIVATE geoworld::debug geoworld::simulation)
    add_test(NAME geoworld.simulation COMMAND geoworld-simulation-test)

    add_executable(geoworld-rules-test tests/unit/rules_test.cpp)
    target_link_libraries(geoworld-rules-test PRIVATE geoworld::rules)
    add_dependencies(geoworld-rules-test geoworld-m3-rule-plugin)
    target_compile_definitions(geoworld-rules-test PRIVATE
        GW_TEST_RULE_PLUGIN_PATH="$<TARGET_FILE:geoworld-m3-rule-plugin>")
    add_test(NAME geoworld.rules COMMAND geoworld-rules-test)

    add_executable(geoworld-ai-test tests/unit/ai_test.cpp)
    target_link_libraries(geoworld-ai-test PRIVATE geoworld::ai)
    add_test(NAME geoworld.ai COMMAND geoworld-ai-test)

    add_executable(geoworld-tooling-test tests/unit/tooling_test.cpp)
    target_link_libraries(geoworld-tooling-test PRIVATE geoworld::tooling)
    add_test(NAME geoworld.tooling COMMAND geoworld-tooling-test)

    add_executable(geoworld-schema-test tests/unit/schema_test.cpp)
    target_link_libraries(geoworld-schema-test PRIVATE geoworld::schema)
    add_test(NAME geoworld.schema COMMAND geoworld-schema-test)

    add_executable(geoworld-world-model-test tests/unit/world_model_test.cpp)
    target_link_libraries(geoworld-world-model-test PRIVATE geoworld::debug geoworld::world_model)
    add_test(NAME geoworld.world_model COMMAND geoworld-world-model-test)

    add_executable(geoworld-observability-test tests/unit/observability_test.cpp)
    target_link_libraries(geoworld-observability-test PRIVATE geoworld::observability)
    add_test(NAME geoworld.observability COMMAND geoworld-observability-test)

    add_executable(geoworld-spatial-test tests/unit/spatial_test.cpp)
    target_link_libraries(geoworld-spatial-test PRIVATE geoworld::spatial)
    add_test(NAME geoworld.spatial COMMAND geoworld-spatial-test)

    add_executable(geoworld-spatial-runtime-test tests/integration/spatial_runtime_test.cpp)
    target_link_libraries(geoworld-spatial-runtime-test PRIVATE geoworld::spatial)
    add_test(NAME geoworld.spatial_runtime COMMAND geoworld-spatial-runtime-test)

    add_executable(geoworld-projection-test tests/unit/projection_test.cpp)
    target_link_libraries(geoworld-projection-test PRIVATE geoworld::projection)
    add_test(NAME geoworld.projection COMMAND geoworld-projection-test)

    if(TARGET geoworld_persistence)
        add_executable(geoworld-persistence-test tests/unit/persistence_test.cpp)
        target_link_libraries(geoworld-persistence-test PRIVATE geoworld::persistence)
        target_compile_options(geoworld-persistence-test PRIVATE -Wall -Wextra)
        add_test(NAME geoworld.persistence COMMAND geoworld-persistence-test)
    endif()

    if(TARGET geoworld_protocol)
        add_executable(geoworld-protocol-test tests/unit/protocol_test.cpp)
        target_link_libraries(geoworld-protocol-test PRIVATE geoworld::protocol)
        add_test(NAME geoworld.protocol COMMAND geoworld-protocol-test)
    endif()

    if(TARGET geoworld_gateway)
        add_executable(geoworld-gateway-test tests/unit/gateway_test.cpp)
        target_link_libraries(geoworld-gateway-test PRIVATE geoworld::gateway)
        add_test(NAME geoworld.gateway COMMAND geoworld-gateway-test)
    endif()

    if(TARGET geoworld_gateway AND TARGET geoworld_world_runtime
            AND TARGET geoworld_stream_client)
        add_executable(geoworld-m4-stream-test tests/integration/m4_stream_test.cpp)
        target_link_libraries(geoworld-m4-stream-test PRIVATE
            geoworld_stream_client
            geoworld::gateway
            geoworld::projection
            geoworld::world_runtime
        )
        add_test(NAME geoworld.m4_stream COMMAND geoworld-m4-stream-test)

        add_executable(geoworld-m4-security-test tests/integration/m4_security_test.cpp)
        target_link_libraries(geoworld-m4-security-test PRIVATE
            geoworld_stream_client
            geoworld::gateway
            geoworld::projection
            geoworld::world_runtime
            gRPC::grpc++
            protobuf::libprotobuf
        )
        # 直接构造 gRPC 无效请求需要生成的控制面类型；生成代码不进入仓库。
        target_include_directories(geoworld-m4-security-test PRIVATE
            "${GW_M4_GENERATED_DIR}")
        if(TARGET geoworldd)
            target_compile_definitions(geoworld-m4-security-test PRIVATE
                GW_GEOWORLD_DAEMON_PATH="$<TARGET_FILE:geoworldd>")
        endif()
        add_test(NAME geoworld.m4_security COMMAND geoworld-m4-security-test)
    endif()

    # M5 durable admission：gateway + persistence + world_runtime 三者齐备才注册。
    if(TARGET geoworld_gateway AND TARGET geoworld_persistence
            AND TARGET geoworld_world_runtime)
        add_executable(geoworld-m5-durable-test tests/integration/m5_durable_test.cpp)
        target_link_libraries(geoworld-m5-durable-test PRIVATE
            geoworld::gateway
            geoworld::persistence
            geoworld::projection
            geoworld::world_runtime
        )
        target_compile_options(geoworld-m5-durable-test PRIVATE -Wall -Wextra)
        add_test(NAME geoworld.m5_durable COMMAND geoworld-m5-durable-test)
    endif()

    if(TARGET geoworld-showcase)
        add_test(NAME geoworld.showcase
            COMMAND geoworld-showcase "${CMAKE_CURRENT_BINARY_DIR}/showcase-test")
        set_tests_properties(geoworld.showcase PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    if(PROJ_FOUND)
        add_executable(geoworld-proj-crosscheck-test tests/unit/proj_crosscheck_test.cpp)
        target_link_libraries(geoworld-proj-crosscheck-test PRIVATE geoworld::spatial PROJ::proj)
        add_test(NAME geoworld.proj_crosscheck COMMAND geoworld-proj-crosscheck-test)
    endif()

    add_executable(geoworld-m1-determinism-test tests/integration/m1_determinism_test.cpp)
    target_link_libraries(geoworld-m1-determinism-test PRIVATE
        geoworld::debug
        geoworld::foundation
        geoworld::simulation
    )
    add_test(NAME geoworld.m1_determinism COMMAND geoworld-m1-determinism-test)

    if(TARGET geoworld_ecs_runtime)
        add_executable(geoworld-ecs-runtime-test tests/unit/ecs_runtime_test.cpp)
        target_link_libraries(geoworld-ecs-runtime-test PRIVATE geoworld::ecs_runtime)
        add_test(NAME geoworld.ecs_runtime COMMAND geoworld-ecs-runtime-test)
    endif()
    if(TARGET geoworld_world_runtime)
        add_executable(geoworld-world-runtime-test tests/integration/world_runtime_test.cpp)
        target_link_libraries(geoworld-world-runtime-test PRIVATE geoworld::world_runtime)
        add_test(NAME geoworld.world_runtime COMMAND geoworld-world-runtime-test)

        add_executable(geoworld-projection-runtime-test
            tests/integration/projection_runtime_test.cpp)
        target_link_libraries(geoworld-projection-runtime-test PRIVATE
            geoworld::projection
            geoworld::world_runtime
        )
        add_test(NAME geoworld.projection_runtime COMMAND geoworld-projection-runtime-test)
    endif()
endif()
