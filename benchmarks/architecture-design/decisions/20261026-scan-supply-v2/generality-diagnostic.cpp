#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

using namespace graph::storage;
namespace {
constexpr uint64_t owners = 16384;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::vector<std::string> labels(uint64_t id, bool changed) {
    if (changed && id % 7U == 0U) return {"changed"};
    if (id % 3U == 0U) return {"alpha", "beta"};
    return {id % 2U == 0U ? "alpha" : "beta"};
}
std::vector<LogicalGraphWriteProperty> fields(uint64_t id, bool changed) {
    const auto adjusted = changed && id % 8U == 0U ? id + 1000U : id;
    std::vector<LogicalGraphWriteProperty> result{{"dense_integer", graph::PropertyValue(int64_t(adjusted))}};
    if (id % 16U == 0U && !changed)
        result.push_back({"sparse_number", id % 32U ? graph::PropertyValue(double(id) + 0.5) : graph::PropertyValue(int64_t(id))});
    switch (id % 4U) {
    case 0: result.push_back({"mixed", graph::PropertyValue(int64_t(id))}); break;
    case 1: result.push_back({"mixed", graph::PropertyValue("mixed")}); break;
    case 2: result.push_back({"mixed", graph::PropertyValue()}); break;
    default: break;
    }
    constexpr std::array<size_t, 4> lengths{0, 2, 63, 1024};
    std::string text(lengths[id % 4U], 'a');
    if (text.size() >= 2U) text[text.size() / 2U] = '\0';
    result.push_back({"text", graph::PropertyValue(std::move(text))});
    return result;
}
auto oracleSource() {
    static auto source = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    return source;
}
std::unique_ptr<LogicalGraphReadSnapshot> readSnapshot(const std::shared_ptr<RelocatableStorageBackend> &backend) {
    // NativeReadSession captures the caller's workspace when the snapshot is
    // created. All oracle snapshots share this one finite source account.
    graph::utils::OwnedAllocationScope allocation(oracleSource());
    return backend->openLogicalGraphReadSnapshot();
}
void check(const LogicalGraphReadSnapshot &snapshot, bool changed) {
    graph::utils::OwnedAllocationScope allocation(oracleSource());
    size_t count = 0;
    snapshot.visitNodes(SIZE_MAX, [&](const LogicalGraphNode &node) {
        ++count; const auto id = node.node_id;
        require(id && id <= owners && !(changed && id % 32U == 0U), "unexpected live ID");
        auto actualLabels = node.labels; auto expectedLabels = labels(id, changed);
        std::sort(actualLabels.begin(), actualLabels.end()); std::sort(expectedLabels.begin(), expectedLabels.end());
        require(actualLabels == expectedLabels, "label oracle differs");
        const auto expected = fields(id, changed);
        require(node.properties.size() == expected.size(), "Property count oracle differs");
        for (const auto &field : expected) {
            const auto found = std::find_if(node.properties.begin(), node.properties.end(), [&](const auto &p) { return p.key == field.key; });
            require(found != node.properties.end() && found->value == field.value, "Property value oracle differs");
        }
        return true;
    });
    require(count == owners - (changed ? owners / 32U : 0U), "live count oracle differs");
}
void query(const std::shared_ptr<RelocatableStorageBackend> &backend, bool changed) {
    uint64_t count = 0, sum = 0;
    for (uint64_t id = 1; id <= owners; ++id) {
        if (changed && id % 32U == 0U) continue;
        ++count; sum += changed && id % 8U == 0U ? id + 1000U : id;
    }
    graph::query::QueryEngine engine(backend);
    std::cerr << "query=dense_sum\n";
    const auto result = engine.execute("MATCH (n) RETURN count(*) AS rows, sum(n.dense_integer) AS total");
    require(result.rowCount() == 1U, "query row count differs");
    require(result.getRows()[0].at("rows").asPrimitive() == graph::PropertyValue(int64_t(count)), "query live oracle differs");
    require(result.getRows()[0].at("total").asPrimitive() == graph::PropertyValue(int64_t(sum)), "query sum oracle differs");
    std::cerr << "query=cold_presence\n";
    const auto presence = engine.execute("MATCH (n) RETURN count(n.sparse_number) AS sparse, count(n.mixed) AS mixed, count(n.text) AS texts");
    require(presence.rowCount() == 1U, "presence query row count differs");
    require(presence.getRows()[0].at("sparse").asPrimitive() == graph::PropertyValue(int64_t(changed ? 0U : owners / 16U)), "sparse oracle differs");
    require(presence.getRows()[0].at("mixed").asPrimitive() == graph::PropertyValue(int64_t(owners / 2U - (changed ? owners / 32U : 0U))), "mixed NULL/missing oracle differs");
    require(presence.getRows()[0].at("texts").asPrimitive() == graph::PropertyValue(int64_t(count)), "text presence oracle differs");
    std::cerr << "query=binary_text_group\n";
    const auto grouped = engine.execute("MATCH (n) RETURN n.text AS key, count(*) AS rows");
    require(grouped.rowCount() == 4U, "binary text grouping cardinality differs");
    for (uint64_t ordinal = 1; ordinal <= 4; ++ordinal) {
        const auto text = fields(ordinal, false).back().value;
        const auto found = std::find_if(grouped.getRows().begin(), grouped.getRows().end(), [&](const auto &row) {
            return row.at("key").asPrimitive() == text;
        });
        require(found != grouped.getRows().end(), "binary text grouping key differs");
        const auto expected = owners / 4U - (changed && ordinal == 4U ? owners / 32U : 0U);
        require(found->at("rows").asPrimitive() == graph::PropertyValue(int64_t(expected)), "binary text grouping count differs");
    }
    uint64_t overlap = 0;
    for (uint64_t id = 3; id <= owners; id += 3U)
        overlap += !(changed && (id % 32U == 0U || id % 7U == 0U));
    std::cerr << "query=label_intersection\n";
    const auto intersect = engine.execute("MATCH (n:alpha:beta) RETURN count(*) AS rows");
    require(intersect.rowCount() == 1U && intersect.getRows()[0].at("rows").asPrimitive() == graph::PropertyValue(int64_t(overlap)), "label overlap oracle differs");
}
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        RelocatableStorageBackendOptions options; options.data_path = argv[2];
        if (std::string_view(argv[1]) == "create") {
            options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
            auto backend = RelocatableStorageBackend::initializeNew(options);
            backend->open();
            for (uint64_t first = 1; first <= owners; first += 128U) {
                std::vector<LogicalGraphNodeCreateRequest> batch;
                for (uint64_t id = first; id < first + 128U; ++id) batch.push_back({labels(id, false), fields(id, false)});
                const auto created = backend->createLogicalNodes(batch);
                require(created.size() == 128U && created.front() == first && created.back() == first + 127U, "creation IDs differ");
            }
            check(*readSnapshot(backend), false); query(backend, false);
            static_cast<void>(backend->verifyIntegrity()); backend->close();
            std::cout << "created_and_verified=" << owners << '\n'; return 0;
        }
        if (std::string_view(argv[1]) == "initial") {
            options.open_mode = OpenMode::OPEN_EXISTING_FILE;
            auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
            auto old = readSnapshot(backend); check(*old, false); query(backend, false);
            backend->close(); std::cout << "initial_oracles_passed\n"; return 0;
        }
        require(std::string_view(argv[1]) == "verify", "unknown action");
        options.open_mode = OpenMode::OPEN_EXISTING_FILE;
        auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
        auto old = readSnapshot(backend); std::cerr << "stage=initial_oracle\n";
        check(*old, false); query(backend, false); std::cerr << "stage=mutations\n";
        for (uint64_t first = 1; first <= owners; first += 32U) {
            LogicalGraphMutationBatch batch;
            for (uint64_t id = first; id < first + 32U; ++id) {
                if (id % 32U == 0U) { batch.node_deletes.push_back({id, false, 0}); continue; }
                const LogicalGraphMutationEntityReference ref{LogicalGraphMutationEntityKind::LGMK_NODE,
                    LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
                if (id % 8U == 0U) batch.property_replacements.push_back({ref, fields(id, true)});
                if (id % 7U == 0U) batch.node_label_replacements.push_back({ref, labels(id, true)});
            }
            static_cast<void>(backend->executeLogicalGraphMutationBatch(batch));
            if (first % 1024U == 1U) std::cerr << "mutation_first=" << first << '\n';
        }
        std::cerr << "stage=pinned_and_current_oracles\n";
        check(*old, false); check(*readSnapshot(backend), true); query(backend, true);
        old.reset(); static_cast<void>(backend->verifyIntegrity()); backend->close();
        options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE; options.buffer_pool_bytes = 4096;
        auto reopened = std::make_shared<RelocatableStorageBackend>(options); reopened->open();
        std::cerr << "stage=small_cache_reopen_oracle\n";
        check(*readSnapshot(reopened), true); query(reopened, true); reopened->close();
        std::cout << "verified_mutations_old_pin_reopen_cache_pressure=" << owners << '\n'; return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
