cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(GeoWorld VERSION 0.1.0 LANGUAGES CXX)

option(GW_BUILD_APPS "Build GeoWorld executables" ON)
option(GW_BUILD_TESTS "Build GeoWorld tests" ON)
option(GW_BUILD_BENCHMARKS "Build GeoWorld benchmarks" ON)
option(GW_REQUIRE_FLECS "Fail configuration when flecs is unavailable" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CTest)

add_library(geoworld_foundation STATIC
    libs/foundation/src/random.cpp
    libs/foundation/src/version.cpp
)
add_library(geoworld::foundation ALIAS geoworld_foundation)
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
)
target_include_directories(geoworld_rules PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/rules/include>
)

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

add_library(geoworld_tooling STATIC
    libs/tooling/src/artifact.cpp
)
add_library(geoworld::tooling ALIAS geoworld_tooling)
target_link_libraries(geoworld_tooling PUBLIC geoworld::foundation geoworld::schema)
target_include_directories(geoworld_tooling PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/tooling/include>
)

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
        geoworld::world_model
    )
    target_include_directories(geoworld_world_runtime PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/world_runtime/include>
    )
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

    add_executable(geoworld-replay apps/geoworld-replay/main.cpp)
    target_link_libraries(geoworld-replay PRIVATE geoworld::simulation)

    add_executable(geoworld-cli apps/geoworld-cli/main.cpp)
    target_link_libraries(geoworld-cli PRIVATE geoworld::world_model)

    add_executable(gw-rule tools/gw-artifact/main.cpp)
    target_compile_definitions(gw-rule PRIVATE GW_ARTIFACT_KIND_RULE=1)
    target_link_libraries(gw-rule PRIVATE geoworld::tooling)

    add_executable(gw-bt tools/gw-artifact/main.cpp)
    target_compile_definitions(gw-bt PRIVATE GW_ARTIFACT_KIND_BT=1)
    target_link_libraries(gw-bt PRIVATE geoworld::tooling)

    add_executable(gw-st tools/gw-artifact/main.cpp)
    target_compile_definitions(gw-st PRIVATE GW_ARTIFACT_KIND_ST=1)
    target_link_libraries(gw-st PRIVATE geoworld::tooling)
endif()

if(GW_BUILD_BENCHMARKS)
    add_executable(geoworld-m1-benchmark benchmarks/m1_core_benchmark.cpp)
    target_link_libraries(geoworld-m1-benchmark PRIVATE geoworld::debug geoworld::simulation)
    add_executable(geoworld-m3-benchmark benchmarks/m3_runtime_benchmark.cpp)
    target_link_libraries(geoworld-m3-benchmark PRIVATE geoworld::ai geoworld::rules)
    if(TARGET geoworld_ecs_runtime AND TARGET EnTT::EnTT)
        add_executable(geoworld-m1-ecs-comparison benchmarks/m1_ecs_comparison.cpp)
        target_link_libraries(geoworld-m1-ecs-comparison PRIVATE
            geoworld::ecs_runtime
            EnTT::EnTT
            $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>
        )
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
    endif()
endif()
