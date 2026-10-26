// Untimed read-only inspection of the registered guard's canonical graph.
// It checks the generator's exact multiset independently of query/index output.
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <vector>
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try {
        using namespace graph::storage;
        RelocatableStorageBackendOptions options;
        options.data_path = argv[1]; options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
        auto snapshot = backend->openLogicalGraphReadSnapshot();
        std::map<uint64_t, int> ordinals;
        size_t nodes = 0, edges = 0, unknown = 0, links = 0;
        snapshot->visitNodes(SIZE_MAX, [&](const auto &node) {
            ++nodes; bool found = false;
            for (const auto &property : node.properties) if (property.key == "id") {
                for (int i = 0; i < 32; ++i) if (property.value == graph::PropertyValue("h" + std::to_string(i))) {
                    found = true; ordinals.emplace(node.node_id, i); break;
                }
            }
            if (!found || std::find(node.labels.begin(), node.labels.end(), "Hub") == node.labels.end()) ++unknown;
            return true;
        });
        using Key = std::tuple<int, int, std::string, std::string>;
        std::multiset<Key> actual, expected;
        std::multiset<std::pair<int, int>> linkPairs;
        std::vector<std::tuple<uint64_t, int, int>> linkIdentities;
        snapshot->visitEdges(SIZE_MAX, [&](const auto &edge) {
            ++edges;
            if (!ordinals.contains(edge.source_node_id) || !ordinals.contains(edge.target_node_id)) { ++unknown; return true; }
            std::string weight = "missing";
            for (const auto &property : edge.properties) if (property.key == "weight") weight = property.value.toString();
            actual.emplace(ordinals.at(edge.source_node_id), ordinals.at(edge.target_node_id), edge.type, weight);
            if (edge.type == "LINK") { ++links; linkPairs.emplace(ordinals.at(edge.source_node_id), ordinals.at(edge.target_node_id)); linkIdentities.emplace_back(edge.edge_id, ordinals.at(edge.source_node_id), ordinals.at(edge.target_node_id)); }
            return true;
        });
        for (int i = 0; i < 512; ++i) {
            const int from = i < 256 ? 0 : i * 17 % 32;
            const int to = i % 17 == 0 ? from : (i * 7 + 3) % 32;
            expected.emplace(from, to, i % 3 == 0 ? "LINK" : "RARE", std::to_string(i % 5));
        }
        int64_t outgoing = 0, both = 0, twohop = 0, uniqueTwohop = 0;
        for (auto [firstId, from, to] : linkIdentities) if (from == 0)
            for (auto [secondId, nextFrom, nextTo] : linkIdentities) { (void)nextTo; if (to == nextFrom && firstId != secondId) ++uniqueTwohop; }
        for (auto [from, to] : linkPairs) {
            if (from == 0) ++outgoing;
            if (from == 0 || to == 0) ++both;
            if (from == 0) for (auto [nextFrom, nextTo] : linkPairs) { (void)nextTo; if (nextFrom == to) ++twohop; }
        }
        std::cout << "{\"query_benchmark\":false,\"nodes\":" << nodes << ",\"edges\":" << edges
                  << ",\"links\":" << links << ",\"unknown\":" << unknown
                  << ",\"registered_graph_matches\":" << (nodes == 32 && unknown == 0 && actual == expected ? "true" : "false")
                  << ",\"actual_graph_out\":" << outgoing << ",\"actual_graph_both\":" << both
                  << ",\"actual_graph_twohop_walks\":" << twohop << ",\"actual_graph_twohop_unique_edges\":" << uniqueTwohop << ",\"index_count\":" << backend->listLogicalIndexes(64).size() << "}\n";
        graph::query::QueryEngine engine(backend);
        for (const auto &[name, query] : std::map<std::string, std::string>{
            {"h0", "MATCH (h:Hub {id:'h0'}) RETURN count(h) AS count"},
            {"out", "MATCH (:Hub {id:'h0'})-[:LINK]->(v:Hub) RETURN count(v) AS count"},
            {"both", "MATCH (:Hub {id:'h0'})-[:LINK]-(v:Hub) RETURN count(v) AS count"},
            {"twohop", "MATCH (:Hub {id:'h0'})-[:LINK]->(:Hub)-[:LINK]->(v:Hub) RETURN count(v) AS count"}}) {
            const auto result = engine.execute(query);
            std::cout << "query=" << name << " rows=" << result.rowCount();
            if (result.rowCount() == 1) std::cout << " count=" << result.getRows().front().at("count").asPrimitive().toString();
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
