cmake_minimum_required(VERSION 3.21)

project(zyx VERSION 0.1.23 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ZYX_BUILD_TESTS "Build ZYX tests" ON)
option(ZYX_BUILD_APPS "Build ZYX CLI and benchmark apps" ON)
option(ZYX_BUILD_PYTHON "Build Python bindings" OFF)
option(ZYX_ENABLE_COVERAGE "Enable LLVM coverage instrumentation" OFF)
option(ZYX_WASM "Configure for Emscripten/WASM build" OFF)
option(ZYX_INSTALL_PKGCONFIG "Install pkg-config metadata" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(ZYX_WASM)
    set(ZYX_BUILD_APPS OFF CACHE BOOL "Build ZYX CLI and benchmark apps" FORCE)
    set(ZYX_BUILD_TESTS OFF CACHE BOOL "Build ZYX tests" FORCE)
endif()

include(Dependencies)
include(SourceSets)
include(CompilerOptions)

configure_file(
    "${PROJECT_SOURCE_DIR}/include/graph/core/ProjectConfig.hpp.in"
    "${PROJECT_BINARY_DIR}/ProjectConfig.hpp"
    @ONLY
)

add_library(zyx_project_options INTERFACE)
zyx_apply_common_options(zyx_project_options)

add_library(zyx_inputxx STATIC ${ZYX_INPUTXX_SOURCES})
target_include_directories(zyx_inputxx
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/inputxx/include>"
)
target_link_libraries(zyx_inputxx PUBLIC zyx_project_options)
add_library(zyx::inputxx ALIAS zyx_inputxx)

add_library(zyx_cypher_parser OBJECT ${ZYX_CYPHER_SOURCES})
target_include_directories(zyx_cypher_parser
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/query/parser/cypher>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/internal>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/public>"
)
target_link_libraries(zyx_cypher_parser PUBLIC zyx_project_options zyx::antlr4)
add_library(zyx::cypher_parser ALIAS zyx_cypher_parser)

add_library(zyx_core STATIC ${ZYX_CORE_SOURCES})
target_include_directories(zyx_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_definitions(zyx_core PUBLIC COVERAGE_SKIP_ZLIB_ERRORS)
target_link_libraries(zyx_core
    PUBLIC
        zyx_project_options
        zyx::zlib
        zyx::boost
        zyx::cli11
        zyx::antlr4
        zyx::cypher_parser
        zyx::inputxx
)
add_library(zyx::core ALIAS zyx_core)

if(NOT ZYX_WASM)
    add_library(zyx SHARED ${ZYX_CORE_SOURCES})
    target_include_directories(zyx
        PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
            "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    target_compile_features(zyx PUBLIC cxx_std_20)
    target_compile_definitions(zyx PRIVATE ZYX_DRIVER_ABI_EXPORTS)
    if(WIN32)
        target_compile_definitions(zyx PUBLIC NOMINMAX NOGDI WIN32_LEAN_AND_MEAN)
    endif()
    target_link_libraries(zyx
        PRIVATE
            zyx_project_options
            zyx::zlib
            zyx::boost
            zyx::cli11
            zyx::antlr4
            zyx::cypher_parser
            zyx::inputxx
    )
    set_target_properties(zyx PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
        OUTPUT_NAME zyx
        MACOSX_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
    add_library(zyx::zyx ALIAS zyx)
endif()

if(ZYX_BUILD_APPS AND NOT ZYX_WASM)
    add_executable(zyx_cli apps/cli/main.cpp)
    set_target_properties(zyx_cli PROPERTIES OUTPUT_NAME zyx)
    target_link_libraries(zyx_cli PRIVATE zyx::core)

    add_executable(zyx_bench apps/benchmark/main.cpp)
    set_target_properties(zyx_bench PROPERTIES OUTPUT_NAME zyx-bench)
    target_include_directories(zyx_bench PRIVATE "${PROJECT_SOURCE_DIR}/apps/benchmark")
    target_link_libraries(zyx_bench PRIVATE zyx::core)
endif()

if(ZYX_BUILD_TESTS AND BUILD_TESTING AND NOT ZYX_WASM)
    zyx_find_test_dependencies()
    add_executable(zyx_test_suite ${ZYX_TEST_SOURCES})
    target_include_directories(zyx_test_suite
        PRIVATE
            "${PROJECT_SOURCE_DIR}/tests/include"
            "${PROJECT_SOURCE_DIR}/lib/inputxx/tests"
            "${PROJECT_SOURCE_DIR}/apps/benchmark"
    )
    target_link_libraries(zyx_test_suite
        PRIVATE
            zyx::core
            zyx::gtest
            zyx::gmock
    )
    add_test(NAME zyx_all_tests
        COMMAND ${CMAKE_COMMAND} -E env sh -c "$<TARGET_FILE:zyx_test_suite> < /dev/null"
    )
    set_tests_properties(zyx_all_tests PROPERTIES TIMEOUT 600)
endif()

if(ZYX_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

include(InstallRules)
