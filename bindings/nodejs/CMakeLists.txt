cmake_minimum_required(VERSION 3.15)
project(zyxdb_addon)

# Node.js addon setup
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages via pkg-config (Conan generates .pc files)
find_package(PkgConfig REQUIRED)
if(PKG_CONFIG_PATH)
    if(WIN32)
        set(ENV{PKG_CONFIG_PATH} "${PKG_CONFIG_PATH};$ENV{PKG_CONFIG_PATH}")
    else()
        set(ENV{PKG_CONFIG_PATH} "${PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()
pkg_check_modules(BOOST REQUIRED boost)
pkg_check_modules(ZLIB REQUIRED zlib)

# ANTLR4 runtime (via pkg-config, Conan generates .pc files)
pkg_check_modules(ANTLR4 REQUIRED antlr4-cppruntime)

# cmake-js provides these variables
include_directories(${CMAKE_JS_INC})

# ZYX paths
set(ZYX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ZYX_SRC_DIR "${ZYX_ROOT}/src")
set(ZYX_INCLUDE_DIR "${ZYX_ROOT}/include")

# =============================================================================
# Core source files (exclude cli)
# =============================================================================
file(GLOB_RECURSE ZYX_SOURCES "${ZYX_SRC_DIR}/*.cpp")

set(ZYX_FILTERED_SOURCES)
foreach(src_file ${ZYX_SOURCES})
    string(FIND "${src_file}" "/cli/" CLI_POS)
    if(CLI_POS EQUAL -1)
        list(APPEND ZYX_FILTERED_SOURCES ${src_file})
    endif()
endforeach()

# =============================================================================
# inputxx library
# =============================================================================
file(GLOB_RECURSE INPUTXX_SOURCES "${ZYX_ROOT}/lib/inputxx/src/*.cpp")

# =============================================================================
# Generate ProjectConfig.hpp
# =============================================================================
set(PROJECT_NAME "zyx")
set(PROJECT_DISPLAY "ZYX")
set(PROJECT_VERSION "0.1.0")
configure_file(
    "${ZYX_INCLUDE_DIR}/graph/core/ProjectConfig.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ProjectConfig.hpp"
    @ONLY
)

# =============================================================================
# Include directories
# =============================================================================
include_directories(
    ${ZYX_INCLUDE_DIR}
    ${ZYX_SRC_DIR}
    ${ZYX_ROOT}
    ${ZYX_SRC_DIR}/query/parser/cypher
    ${ZYX_SRC_DIR}/query/parser/cypher/generated
    ${ZYX_SRC_DIR}/query/parser/cypher/helpers/internal
    ${ZYX_SRC_DIR}/query/parser/cypher/helpers/public
    ${ZYX_ROOT}/lib/inputxx/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${ANTLR4_INCLUDE_DIRS}
    ${BOOST_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/node_modules/node-addon-api
)

# Compile definitions
add_definitions(-DCOVERAGE_SKIP_ZLIB_ERRORS -DNAPI_VERSION=8)

# =============================================================================
# Build the addon
# =============================================================================
add_library(zyxdb SHARED
    src/addon.cpp
    ${ZYX_FILTERED_SOURCES}
    ${INPUTXX_SOURCES}
)

set_target_properties(zyxdb PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    CXX_STANDARD 20
)

# Link libraries
target_link_libraries(zyxdb
    ${CMAKE_JS_LIB}
    ${ZLIB_LINK_LIBRARIES}
    ${ANTLR4_LINK_LIBRARIES}
)

# Platform-specific settings
if(MSVC)
    target_compile_options(zyxdb PRIVATE /W4)
else()
    target_compile_options(zyxdb PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
