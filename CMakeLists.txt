cmake_minimum_required(VERSION 3.21)

project(zyx VERSION 0.1.29 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ZYX_BUILD_TESTS "Build ZYX tests" ON)
option(ZYX_BUILD_APPS "Build ZYX CLI and benchmark apps" ON)
option(ZYX_BUILD_PYTHON "Build Python bindings" OFF)
option(ZYX_BUILD_SHARED_LIBRARY "Build the public shared ZYX library" ON)
option(ZYX_BUILD_STATIC_LIBRARY "Build the public static ZYX library" OFF)
option(ZYX_ENABLE_COVERAGE "Enable LLVM coverage instrumentation" OFF)
option(ZYX_WASM "Configure for Emscripten/WASM build" OFF)
option(ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT "Build isolated hybrid-property format prototype" OFF)
option(ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT "Build isolated complete-scan format prototype" OFF)
if(ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT AND NOT ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT)
    message(FATAL_ERROR "Scan-path format requires the hybrid-property format option")
endif()
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
# ANTLR's prebuilt runtime resets ParseTreeTracker from an uninstrumented
# translation unit. Disable only libc++ container annotations in the generated
# parser when this target itself is AddressSanitizer-instrumented; otherwise a
# valid reset can leave stale poison state in the runtime-owned vector.
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
   CMAKE_CXX_FLAGS MATCHES "(^|[ \t])-fsanitize=[^ \t]*address")
    target_compile_definitions(zyx_cypher_parser PRIVATE _LIBCPP_HAS_NO_ASAN=1)
endif()
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
target_compile_definitions(zyx_core PUBLIC COVERAGE_SKIP_ZLIB_ERRORS
    ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=$<BOOL:${ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT}>
    ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=$<BOOL:${ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT}>)
target_link_libraries(zyx_core
    PUBLIC
        zyx_project_options
        zyx::zlib
        zyx::large_value_codecs
        zyx::boost
        zyx::cli11
        zyx::antlr4
        zyx::cypher_parser
        zyx::inputxx
)
add_library(zyx::core ALIAS zyx_core)

function(zyx_configure_public_library target)
    target_include_directories(${target}
        PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
            "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(WIN32)
        target_compile_definitions(${target} PUBLIC NOMINMAX NOGDI WIN32_LEAN_AND_MEAN)
    endif()
    target_link_libraries(${target}
        PRIVATE
            zyx_project_options
            zyx::zlib
            zyx::large_value_codecs
            zyx::boost
            zyx::cli11
            zyx::antlr4
            zyx::cypher_parser
            zyx::inputxx
    )
endfunction()

if(NOT ZYX_WASM AND ZYX_BUILD_SHARED_LIBRARY)
    add_library(zyx SHARED ${ZYX_CORE_SOURCES})
    target_compile_definitions(zyx PRIVATE ZYX_DRIVER_ABI_EXPORTS)
    zyx_configure_public_library(zyx)
    set_target_properties(zyx PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
        OUTPUT_NAME zyx
        MACOSX_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
    add_library(zyx::zyx ALIAS zyx)
endif()

if(NOT ZYX_WASM AND ZYX_BUILD_STATIC_LIBRARY)
    add_library(zyx_static STATIC ${ZYX_CORE_SOURCES})
    zyx_configure_public_library(zyx_static)
    set_target_properties(zyx_static PROPERTIES OUTPUT_NAME zyx)
    add_library(zyx::zyx_static ALIAS zyx_static)
endif()

if(ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT AND NOT ZYX_WASM)
    add_executable(zyx_scan_convert EXCLUDE_FROM_ALL apps/scan_convert/main.cpp)
    set_target_properties(zyx_scan_convert PROPERTIES OUTPUT_NAME zyx-scan-convert)
    target_link_libraries(zyx_scan_convert PRIVATE zyx::core)
endif()

if(ZYX_BUILD_APPS AND NOT ZYX_WASM)
    add_executable(zyx_cli apps/cli/main.cpp)
    set_target_properties(zyx_cli PROPERTIES OUTPUT_NAME zyx)
    target_link_libraries(zyx_cli PRIVATE zyx::core)

    add_executable(zyx_bench apps/benchmark/main.cpp)
    set_target_properties(zyx_bench PROPERTIES OUTPUT_NAME zyx-bench)
    target_include_directories(zyx_bench PRIVATE "${PROJECT_SOURCE_DIR}/apps/benchmark")
    target_link_libraries(zyx_bench PRIVATE zyx::core)

    add_executable(zyx_compare_bench apps/compare_benchmark/main.cpp)
    set_target_properties(zyx_compare_bench PROPERTIES OUTPUT_NAME zyx-compare-bench)
    target_link_libraries(zyx_compare_bench PRIVATE zyx::core)
    if(WIN32)
        target_link_libraries(zyx_compare_bench PRIVATE psapi)
    endif()

    add_executable(zyx_workload_contract_worker benchmarks/graphdb-comparison/workload-contract/native-worker.cpp)
    set_target_properties(zyx_workload_contract_worker PROPERTIES OUTPUT_NAME zyx-workload-contract-worker)
    target_link_libraries(zyx_workload_contract_worker PRIVATE zyx::core)

    add_executable(zyx_vector_calibrate apps/vector_calibration/main.cpp)
    set_target_properties(zyx_vector_calibrate PROPERTIES OUTPUT_NAME zyx-vector-calibrate)
    target_link_libraries(zyx_vector_calibrate PRIVATE zyx::core)
    if(WIN32)
        target_link_libraries(zyx_vector_calibrate PRIVATE psapi)
    endif()

    add_executable(zyx_vector_build_calibrate apps/vector_build_calibration/main.cpp)
    set_target_properties(zyx_vector_build_calibrate PROPERTIES OUTPUT_NAME zyx-vector-build-calibrate)
    target_link_libraries(zyx_vector_build_calibrate PRIVATE zyx::core)
    if(WIN32)
        target_link_libraries(zyx_vector_build_calibrate PRIVATE psapi)
    endif()

    add_executable(zyx_large_value_calibrate apps/large_value_calibration/main.cpp)
    set_target_properties(zyx_large_value_calibrate PROPERTIES OUTPUT_NAME zyx-large-value-calibrate)
    target_link_libraries(zyx_large_value_calibrate PRIVATE zyx::core)
    if(WIN32)
        target_link_libraries(zyx_large_value_calibrate PRIVATE psapi)
    endif()

    add_executable(zyx_bulk_edge_calibrate apps/bulk_edge_calibration/main.cpp)
    set_target_properties(zyx_bulk_edge_calibrate PROPERTIES OUTPUT_NAME zyx-bulk-edge-calibrate)
    target_link_libraries(zyx_bulk_edge_calibrate PRIVATE zyx::core)
    if(WIN32)
        target_link_libraries(zyx_bulk_edge_calibrate PRIVATE psapi)
    endif()

    add_subdirectory(benchmarks/leiden)
endif()

if(ZYX_BUILD_TESTS AND BUILD_TESTING AND NOT ZYX_WASM)
    zyx_find_test_dependencies()
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    # Compile fault-injection seams into this test-only core artifact. Release
    # builds retain direct native I/O with no hook dispatch on write hot paths.
    target_compile_definitions(zyx_core PRIVATE ZYX_ENABLE_STORAGE_TEST_HOOKS=1)
    # Google Mock generates zero-region expectation helpers in these vendored
    # test units. LLVM records no profile counter for them, which otherwise
    # produces a spurious profile mismatch warning while their sources are
    # already excluded from the production coverage report.
    if(ZYX_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        set_source_files_properties(
            "${PROJECT_SOURCE_DIR}/lib/inputxx/tests/test_Session.cpp"
            "${PROJECT_SOURCE_DIR}/lib/inputxx/tests/frontend/test_Editor.cpp"
            PROPERTIES COMPILE_OPTIONS "-fno-profile-instr-generate;-fno-coverage-mapping"
        )
    endif()
    # Explicitly selected process-crash probe; neither the default build nor
    # CTest launches it. Its Python runner owns only fresh temporary databases.
    add_executable(zyx_query_scratch_crash_worker EXCLUDE_FROM_ALL
        tests/support/QueryScratchCrashWorker.cpp)
    target_link_libraries(zyx_query_scratch_crash_worker PRIVATE zyx::core)
    # Explicit process isolation for replacement-new fault injection.
    add_executable(zyx_transaction_resource_fault_worker EXCLUDE_FROM_ALL
        tests/support/TransactionResourceFaultWorker.cpp)
    target_link_libraries(zyx_transaction_resource_fault_worker PRIVATE zyx::core)
    add_executable(zyx_test_suite ${ZYX_TEST_SOURCES})
    target_include_directories(zyx_test_suite
        PRIVATE
            "${PROJECT_SOURCE_DIR}/tests/include"
            "${PROJECT_SOURCE_DIR}/lib/inputxx/tests"
            "${PROJECT_SOURCE_DIR}/apps/benchmark"
    )
    target_compile_definitions(zyx_test_suite PRIVATE ZYX_ENABLE_STORAGE_TEST_HOOKS=1)
    target_link_libraries(zyx_test_suite
        PRIVATE
            zyx::core
            zyx::gtest
            zyx::gmock
    )
    # The runner creates an isolated process group and reaps its descendants.
    # This prevents debugger and crash-handler children from surviving CTest on
    # both POSIX and native Windows hosts.
    add_test(
        NAME zyx_all_tests
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/run_test_process.py"
                --timeout 600 -- $<TARGET_FILE:zyx_test_suite>
    )
    # Leave the runner enough time to complete its bounded SIGTERM/SIGKILL or
    # taskkill cleanup before CTest applies its own outer timeout.
    set_tests_properties(zyx_all_tests PROPERTIES TIMEOUT 620)
endif()

if(ZYX_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

include(InstallRules)
