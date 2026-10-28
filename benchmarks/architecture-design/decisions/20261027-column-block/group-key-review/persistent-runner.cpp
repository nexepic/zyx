#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using graph::PropertyValue;
using Key = std::pair<std::string, int64_t>;
static std::string key(const PropertyValue &value) {
    return std::to_string(int(value.getType())) + ":" + value.toString();
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    const size_t count = std::stoull(argv[2]), mode = std::stoull(argv[3]);
    if ((count != 512 && count != 1536) || mode >= 5) return 2;
    std::map<Key, int64_t> expected;
    for (size_t id = 1; id <= count; ++id) {
        const size_t g = (mode == 1 || mode == 3) ? id : (id * 37 + 19) % 6;
        PropertyValue text;
        if (mode == 4 && (id % 7 == 0 || id % 7 == 1)) text = PropertyValue();
        else if (mode == 4 && id % 7 == 2) text = PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(g))});
        else text = PropertyValue(std::string((mode == 2 || mode == 3) ? 1024 : 8, char('a' + g % 6)));
        ++expected[{key(text), int64_t(g)}];
    }
    graph::storage::RelocatableStorageBackendOptions opts; opts.data_path = argv[1];
    opts.open_mode = graph::storage::OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    auto backend = std::make_shared<graph::storage::RelocatableStorageBackend>(opts);
    auto start = std::chrono::steady_clock::now(); backend->open();
    const auto openNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    graph::query::QueryEngine engine(backend);
    const std::string query = "MATCH (n:Nodes) RETURN n.text AS a, n.number AS b, count(*) AS rows";
    for (int iteration = 0; iteration < 11; ++iteration) {
        std::string command;
        if (!std::getline(std::cin, command) || command != "run") return 5;
        start = std::chrono::steady_clock::now();
        const auto result = engine.execute(query);
        std::map<Key, int64_t> actual;
        for (const auto &row : result.getRows()) {
            const auto &a = row.at("a").asPrimitive();
            const auto &b = row.at("b").asPrimitive();
            const auto &n = row.at("rows").asPrimitive();
            if (!actual.emplace(Key{key(a), std::get<int64_t>(b.getVariant())}, std::get<int64_t>(n.getVariant())).second) return 3;
        }
        if (actual != expected) { std::cerr << "complete oracle mismatch\n"; return 4; }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << "sample=" << iteration << " ns=" << ns << " groups=" << actual.size() << " open_ns=" << openNs << std::endl;
    }
    backend->close();
}
