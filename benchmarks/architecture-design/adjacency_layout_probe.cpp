#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// Read-layout experiment only: no tree seek, I/O, transactions, or writes.
// All variants traverse the same outgoing edges and produce the same checksum.
struct Edge {
    uint64_t source, target, type, nextOut, prevOut, nextIn, prevIn, id;
};
struct Cover { uint64_t id, target, type; };
static_assert(sizeof(Edge) == 64 && sizeof(Cover) == 24);
constexpr uint64_t END = UINT64_MAX;
volatile uint64_t sink = 0;

int main() {
    constexpr size_t edgeCount = 1U << 20;
    constexpr size_t samples = 7;
    std::cout << "layout,degree,variant,edges_per_sample,median_ns_per_edge,min_ns_per_edge,checksum,samples_ns_per_edge\n";
    for (bool scattered : {false, true}) {
        for (size_t degree : {4U, 32U, 512U}) {
            const size_t nodes = edgeCount / degree;
            std::mt19937_64 rng(42);
            std::vector<uint64_t> permutation(edgeCount);
            std::iota(permutation.begin(), permutation.end(), 0);
            if (scattered) std::shuffle(permutation.begin(), permutation.end(), rng);
            std::vector<Edge> edges(edgeCount);
            std::vector<uint64_t> ids(edgeCount), heads(nodes), queries(nodes);
            std::vector<Cover> covers(edgeCount);
            for (size_t n = 0; n < nodes; ++n) {
                heads[n] = permutation[n * degree];
                for (size_t j = 0; j < degree; ++j) {
                    const size_t pos = n * degree + j;
                    const auto physical = permutation[pos];
                    const auto target = rng() % nodes;
                    const auto type = rng() % 8;
                    edges[physical] = {n, target, type,
                        j + 1 == degree ? END : permutation[pos + 1],
                        j == 0 ? END : permutation[pos - 1], END, END, physical};
                    ids[pos] = physical;
                    covers[pos] = {physical, target, type};
                }
            }
            std::iota(queries.begin(), queries.end(), 0);
            std::shuffle(queries.begin(), queries.end(), rng);
            queries.resize(std::min<size_t>(4096, nodes));
            const size_t repeats = std::max<size_t>(1, (1U << 20) / (queries.size() * degree));
            const size_t visits = repeats * queries.size() * degree;
            const auto run = [&](size_t variant) {
                uint64_t sum = 0;
                for (size_t rep = 0; rep < repeats; ++rep) {
                    for (const auto node : queries) {
                        if (variant == 0) {
                            for (uint64_t id = heads[node]; id != END; id = edges[id].nextOut) {
                                const auto& edge = edges[id];
                                sum += (edge.target ^ edge.id) + edge.type;
                            }
                        } else if (variant == 1) {
                            const auto begin = node * degree;
                            for (size_t j = begin; j < begin + degree; ++j) {
                                const auto& edge = edges[ids[j]];
                                sum += (edge.target ^ edge.id) + edge.type;
                            }
                        } else {
                            const auto begin = node * degree;
                            for (size_t j = begin; j < begin + degree; ++j) {
                                const auto& edge = covers[j];
                                sum += (edge.target ^ edge.id) + edge.type;
                            }
                        }
                    }
                }
                return sum;
            };
            std::array<std::vector<double>, 3> timings;
            uint64_t expected = run(0);
            for (int round = -2; round < static_cast<int>(samples); ++round) {
                for (size_t step = 0; step < 3; ++step) {
                    const size_t variant = (step + static_cast<size_t>(round + 2)) % 3;
                    const auto start = std::chrono::steady_clock::now();
                    const auto sum = run(variant);
                    const auto end = std::chrono::steady_clock::now();
                    if (sum != expected) throw std::runtime_error("checksum mismatch");
                    sink = sum;
                    if (round >= 0) timings[variant].push_back(
                        std::chrono::duration<double, std::nano>(end - start).count() / visits);
                }
            }
            const std::array<const char*, 3> names = {"edge_link", "id_then_edge", "covering_array"};
            for (size_t v = 0; v < 3; ++v) {
                const auto raw = timings[v];
                std::sort(timings[v].begin(), timings[v].end());
                std::cout << (scattered ? "scattered" : "clustered") << ',' << degree << ','
                          << names[v] << ',' << visits << ',' << timings[v][samples / 2]
                          << ',' << timings[v][0] << ',' << expected << ',';
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (i) std::cout << ';';
                    std::cout << raw[i];
                }
                std::cout << '\n';
            }
        }
    }
}
