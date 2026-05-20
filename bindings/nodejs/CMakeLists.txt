cmake_minimum_required(VERSION 3.21)
project(zyxdb_addon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(ZYX_NODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ZYX_BUILD_TESTS OFF CACHE BOOL "Build ZYX tests" FORCE)
set(ZYX_BUILD_APPS OFF CACHE BOOL "Build ZYX apps" FORCE)
set(ZYX_BUILD_PYTHON OFF CACHE BOOL "Build ZYX Python bindings" FORCE)
add_subdirectory("${ZYX_NODE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/zyx_core_build")

add_library(zyxdb SHARED src/addon.cpp)
set_target_properties(zyxdb PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    CXX_STANDARD 20
)

target_include_directories(zyxdb PRIVATE
    ${CMAKE_JS_INC}
    "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-addon-api"
    "${ZYX_NODE_ROOT}/include"
)
target_compile_definitions(zyxdb PRIVATE NAPI_VERSION=8)
target_link_libraries(zyxdb PRIVATE ${CMAKE_JS_LIB} zyx::core)

if(MSVC)
    target_compile_options(zyxdb PRIVATE /W4)
else()
    target_compile_options(zyxdb PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
