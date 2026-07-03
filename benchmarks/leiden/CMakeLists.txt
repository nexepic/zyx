# Leiden algorithm benchmark: measures CSR build / Leiden runtime / peak RSS
# on synthetic graphs (clique-chain and Erdős–Rényi random). Capped at 100k
# nodes so it stays cheap to run repeatedly.
#
# Links zyx::core (which transitively brings Boost headers used for the
# uuid-based temp database filename). No extra find_package needed.

add_executable(leiden_bench leiden_bench.cpp)
set_target_properties(leiden_bench PROPERTIES OUTPUT_NAME leiden-bench)
target_link_libraries(leiden_bench PRIVATE zyx::core)