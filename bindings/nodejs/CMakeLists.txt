cmake_minimum_required(VERSION 3.15)
project(zyxdb_addon)

# Node.js addon setup
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED)

# ANTLR4 runtime
find_library(ANTLR4_LIBRARY NAMES antlr4-runtime REQUIRED)
find_path(ANTLR4_INCLUDE_DIR antlr4-runtime.h PATH_SUFFIXES antlr4-runtime)

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
    ${ANTLR4_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
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
    ZLIB::ZLIB
    ${ANTLR4_LIBRARY}
)

# Platform-specific settings
if(MSVC)
    target_compile_options(zyxdb PRIVATE /W4)
else()
    target_compile_options(zyxdb PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
