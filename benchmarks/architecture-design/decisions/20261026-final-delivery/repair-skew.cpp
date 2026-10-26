// Restore the originally registered graph on a fresh copy; never alter the source.
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        using namespace graph::storage;
        const std::filesystem::path source = argv[1], target = argv[2];
        if (std::filesystem::exists(target)) throw std::runtime_error("repair requires a fresh destination");
        if (std::filesystem::file_size(source) > UINT64_C(1024) * 1024 * 1024)
            throw std::runtime_error("fixture copy exceeds the aggregate file allowance");
        std::filesystem::copy_file(source, target);
        RelocatableStorageBackendOptions options;
        options.data_path = target; options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        options.maximum_database_file_bytes = UINT64_C(2) * 1024 * 1024 * 1024 - std::filesystem::file_size(source);
        RelocatableStorageBackend backend(options); backend.open();
        auto pin = backend.openLogicalGraphReadSnapshot();
        std::array<uint64_t, 32> ids{}; size_t nodes = 0, edges = 0;
        pin->visitNodes(SIZE_MAX, [&](const auto &node) {
            ++nodes;
            if (std::find(node.labels.begin(), node.labels.end(), "Hub") == node.labels.end())
                throw std::runtime_error("registered Hub label is missing");
            for (const auto &property : node.properties) if (property.key == "id")
                for (size_t ordinal = 0; ordinal < ids.size(); ++ordinal)
                    if (property.value == graph::PropertyValue("h" + std::to_string(ordinal))) {
                        if (ids[ordinal]) throw std::runtime_error("registered Hub id is duplicated");
                        ids[ordinal] = node.node_id;
                    }
            return true;
        });
        pin->visitEdges(SIZE_MAX, [&](const auto &) { ++edges; return true; });
        if (nodes != ids.size() || edges || std::find(ids.begin(), ids.end(), 0U) != ids.end())
            throw std::runtime_error("source is not the independently diagnosed node-only fixture");
        pin.reset(); size_t created = 0;
        // Preserve the original generator's LINK-before-RARE insertion order.
        // Public API batches are bounded to 64; graph contents are unchanged.
        for (const bool link : {true, false}) {
            std::vector<LogicalGraphEdgeCreateRequest> batch;
            const auto flush = [&] {
                if (batch.empty()) return;
                const auto result = backend.createLogicalEdges(batch);
                if (result.size() != batch.size()) throw std::runtime_error("registered edge batch was incomplete");
                created += result.size(); batch.clear();
            };
            for (size_t i = 0; i < 512; ++i) if ((i % 3 == 0) == link) {
                const size_t from = i < 256 ? 0 : i * 17 % 32;
                const size_t to = i % 17 == 0 ? from : (i * 7 + 3) % 32;
                batch.push_back({ids[from], ids[to], link ? "LINK" : "RARE",
                                 {{"weight", graph::PropertyValue(int64_t(i % 5))}}});
                if (batch.size() == 64) flush();
            }
            flush();
        }
        static_cast<void>(backend.verifyIntegrity()); backend.close();
        if (created != 512) throw std::runtime_error("registered graph restoration lost edges");
        std::cout << "{\"query_benchmark\":false,\"nodes\":32,\"created_edges\":" << created
                  << ",\"target_bytes\":" << std::filesystem::file_size(target) << "}\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
