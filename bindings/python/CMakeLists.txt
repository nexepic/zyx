find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(zyxdb_core MODULE src/zyxdb/_core.cpp)
set_target_properties(zyxdb_core PROPERTIES
    OUTPUT_NAME _core
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/zyxdb"
)

target_include_directories(zyxdb_core PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_BINARY_DIR}"
)
if(UNIX AND NOT APPLE)
    target_link_libraries(zyxdb_core PRIVATE
        -Wl,--start-group
        zyx::core
        zyx::cypher_parser
        -Wl,--end-group
    )
else()
    target_link_libraries(zyxdb_core PRIVATE zyx::core)
endif()

if(MSVC)
    target_compile_options(zyxdb_core PRIVATE /FI"${CMAKE_CURRENT_SOURCE_DIR}/undef_python_tokens.h")
else()
    target_compile_options(zyxdb_core PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/undef_python_tokens.h")
endif()

install(TARGETS zyxdb_core LIBRARY DESTINATION zyxdb)
