# Leiden benchmark over the storage-neutral logical projection/algorithm path.
# It is intentionally in-memory: persistence calibration is measured by the
# native storage tools, while this binary isolates projection and algorithm cost.

add_executable(leiden_bench leiden_bench.cpp)
set_target_properties(leiden_bench PROPERTIES OUTPUT_NAME leiden-bench)
target_link_libraries(leiden_bench PRIVATE zyx::core)
if(WIN32)
    target_link_libraries(leiden_bench PRIVATE psapi)
endif()
