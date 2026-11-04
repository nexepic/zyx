/**
 * @file test_RelocatableLogicalGraphReadSnapshot.cpp
 * @brief Public logical-read snapshot continuation-cache integration tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "CypherParserImpl.hpp"
#include <thread>
#include <cstdio>
#include <exception>
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

using namespace relocatable_storage_backend_test;


TEST_F(RelocatableStorageBackendTest, NativeEdgeCellsKeepTightAndRetryReadsBounded) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"EdgeCell"}, {}}});
    std::vector<LogicalGraphEdgeCreateRequest> requests;
    for (size_t i = 0; i < 6U; ++i)
        requests.push_back({nodes.front(), nodes.front(), "EDGE_CELL", {{"payload", PropertyValue(std::string(3000U, static_cast<char>('a' + i)))}}});
    const auto edgeIds = backend.createLogicalEdges(requests);
    ASSERT_EQ(edgeIds.size(), 6U);
	{
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsPropertyReadAdmission());
        ASSERT_TRUE(snapshot->supportsResumableEdgePropertyCells());
		const std::array<uint64_t, 2U> ids{edgeIds[0], edgeIds[1]};
		const std::array<std::string_view, 1U> keys{"payload"};
		const bool wasEnabled = graph::debug::PerfTrace::areValuesEnabled();
		struct Restore { bool enabled; ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); graph::debug::PerfTrace::reset(); } } restore{wasEnabled};
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		size_t admitted = 0U, admissionCalls = 0U;
		EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSortedWithAdmission(ids, keys, [&](size_t bytes) {
			++admissionCalls;
			if (bytes > 4096U - admitted) throw std::runtime_error("test source admission denied");
			admitted += bytes;
		})), std::runtime_error);
		EXPECT_GE(admissionCalls, 2U);
		EXPECT_LE(admitted, 4096U);
		const auto rejectedTrace = graph::debug::PerfTrace::snapshotAndReset();
		if (const auto decoded = rejectedTrace.find("native_graph.read.selected_values_decoded"); decoded != rejectedTrace.end())
			EXPECT_LE(decoded->second.totalValue, 1);
	}
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH ()-[e:EDGE_CELL]->() RETURN e.payload AS payload");
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{
		.maximum_node_records = 16U, .maximum_node_page_records = 4U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 16U,
		.maximum_edge_records = 16U, .maximum_adjacency_page_records = 4U,
		.maximum_property_entries = 2U, .maximum_rows = 16U, .maximum_distinct_rows = 16U};
	for (const size_t budget : {size_t{4096U}, size_t{6000U}, size_t{100000U}}) {
		SCOPED_TRACE(budget); bounds.maximum_materialized_bytes = budget;
		const bool wasEnabled = graph::debug::PerfTrace::areValuesEnabled();
		struct Restore { bool enabled; ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); graph::debug::PerfTrace::reset(); } } restore{wasEnabled};
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		size_t observedPeak = 0U;
		graph::debug::PerfTrace::ScopedObserver observer(&observedPeak, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
			if (key == "logical_plan.materialized_budget_peak_bytes" && value > 0)
				*static_cast<size_t *>(owner) = std::max(*static_cast<size_t *>(owner), static_cast<size_t>(value));
		});
		backend.resetIoStats();
		size_t delivered = 0U; std::array<size_t, 6U> seen{};
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
			++delivered;
			const auto &value = std::get<std::string>(std::get<PropertyValue>(row.values.at("payload")).getVariant());
			EXPECT_EQ(value.size(), 3000U);
			if (!value.empty() && value.front() >= 'a' && value.front() <= 'f') {
				++seen[static_cast<size_t>(value.front() - 'a')];
				EXPECT_TRUE(std::ranges::all_of(value, [&](char character) { return character == value.front(); }));
			} else ADD_FAILURE() << "unexpected payload";
			return true;
		})));
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		::testing::Test::RecordProperty(budget == 4096U ? "tight_data_read_bytes" : budget == 6000U ? "retry_data_read_bytes" : "ample_data_read_bytes", static_cast<int>(backend.ioStats().data.read_bytes));
		EXPECT_EQ(delivered, 6U);
		EXPECT_EQ(seen, (std::array<size_t, 6U>{1U, 1U, 1U, 1U, 1U, 1U}));
		EXPECT_GT(observedPeak, 0U);
		EXPECT_LE(observedPeak, budget);
		::testing::Test::RecordProperty(budget == 4096U ? "tight_logical_peak_bytes" : budget == 6000U ? "retry_logical_peak_bytes" : "ample_logical_peak_bytes", static_cast<int>(observedPeak));
		ASSERT_TRUE(trace.contains("logical_plan.property_source.admitted_bytes"));
		EXPECT_GE(trace.at("logical_plan.property_source.admitted_bytes").totalValue, 18000);
		ASSERT_TRUE(trace.contains("native_graph.read.selected_values_decoded"));
		EXPECT_EQ(trace.at("native_graph.read.selected_values_decoded").totalValue, 6);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StagedOverlayCellsMergeBaseOwnersAndRejectRevisionChanges) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(std::move(options)); backend.open();
    const auto nodes = backend.createLogicalNodes({
        {{"OverlayCell"}, {{"payload", PropertyValue(std::string(3000U, 'a'))}}},
        {{"OverlayCell"}, {{"payload", PropertyValue("old")}}}});
    const auto edges = backend.createLogicalEdges({
        {nodes[0], nodes[0], "OVERLAY_CELL", {{"payload", PropertyValue(std::string(3000U, 'a'))}}},
        {nodes[1], nodes[1], "OVERLAY_CELL", {{"payload", PropertyValue("old")}}}});
    auto transaction = backend.beginLogicalGraphTransaction(false);
    LogicalGraphTransactionOverlay overlay(*transaction);
    overlay.replaceLogicalNodeProperties({{nodes[1], {{"payload", PropertyValue(std::string(3000U, 'b'))}}}});
    overlay.replaceLogicalEdgeProperties({{edges[1], {{"payload", PropertyValue(std::string(3000U, 'b'))}}}});
    const auto created = overlay.createLogicalNodes({{{"OverlayCell"}, {{"payload", PropertyValue(std::string(3000U, 'c'))}}}}).front();
    const auto createdEdge = overlay.createLogicalEdges({{created, created, "OVERLAY_CELL", {{"payload", PropertyValue(std::string(3000U, 'c'))}}}}).front();
    const std::array<uint64_t, 3U> nodeIds{nodes[0], nodes[1], created};
    const std::array<uint64_t, 3U> edgeIds{edges[0], edges[1], createdEdge};
    const std::array<std::string_view, 1U> keys{"payload"};
    for (const bool vectorView : {false, true}) {
        std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorView
            ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
            : overlay.openLogicalGraphReadSnapshot();
        ASSERT_TRUE(snapshot->supportsResumableNodePropertyCells());
        ASSERT_TRUE(snapshot->supportsResumableEdgePropertyCells());
        for (const bool edge : {false, true}) {
            SCOPED_TRACE(edge);
            SCOPED_TRACE(vectorView);
            const auto &ids = edge ? edgeIds : nodeIds;
            const auto read = [&](std::span<const uint64_t> owners, const auto &sink, const auto &admit) {
                if (edge) snapshot->readEdgePropertyCellsSortedWithAdmission(owners, keys, sink, admit);
                else snapshot->readNodePropertyCellsSortedWithAdmission(owners, keys, sink, admit);
            };
            std::vector<PropertyValue> values;
            size_t admitted = 0U;
            EXPECT_THROW(read(ids, [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                EXPECT_EQ(row, 0U); EXPECT_EQ(column, 0U); ASSERT_TRUE(value); values.push_back(std::move(*value));
            }, [&](size_t bytes) {
                if (bytes > 4096U - admitted) throw std::runtime_error("test partial admission refusal");
                admitted += bytes;
            }), std::runtime_error);
            ASSERT_EQ(values.size(), 1U);
            EXPECT_EQ(values[0], PropertyValue(std::string(3000U, 'a')));
            // The caller owns and charges completed private values. Resume only
            // the pending owners; no downstream result was delivered above.
            read(std::span<const uint64_t>(ids).subspan(1), [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                EXPECT_EQ(column, 0U); ASSERT_TRUE(value);
                EXPECT_EQ(*value, PropertyValue(std::string(3000U, static_cast<char>('b' + row))));
            }, [](size_t) {});
        }
    }
    graph::parser::cypher::CypherParserImpl parser;
    execution::LogicalPlanReadRuntime runtime(overlay);
    execution::LogicalPlanReadRuntimeBounds bounds{
        .maximum_node_records = 16U, .maximum_node_page_records = 4U,
        .maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 16U,
        .maximum_edge_records = 16U, .maximum_adjacency_page_records = 4U,
        .maximum_property_entries = 2U, .maximum_rows = 16U,
        .maximum_distinct_rows = 16U, .maximum_materialized_bytes = 6000U};
    for (const auto query : {"MATCH (n:OverlayCell) RETURN n.payload AS payload",
                            "MATCH ()-[e:OVERLAY_CELL]->() RETURN e.payload AS payload"}) {
        const auto plan = parser.parseToLogical(query);
        std::array<size_t, 3U> seen{};
        ASSERT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
            const auto &text = std::get<std::string>(std::get<PropertyValue>(row.values.at("payload")).getVariant());
            EXPECT_EQ(text.size(), 3000U);
            if (!text.empty() && text.front() >= 'a' && text.front() <= 'c') ++seen[text.front() - 'a'];
            else ADD_FAILURE() << "unexpected staged payload";
            return true;
        })));
        EXPECT_EQ(seen, (std::array<size_t, 3U>{1U, 1U, 1U}));
    }
    auto snapshot = overlay.openLogicalGraphReadSnapshot();
    bool mutated = false;
    EXPECT_THROW(snapshot->readNodePropertyCellsSortedWithAdmission(nodeIds, keys,
        [&](size_t, size_t, std::optional<PropertyValue>) {
            mutated = true;
            overlay.replaceLogicalNodeProperties({{nodes[1], {{"payload", PropertyValue("changed")}}}});
        }, [](size_t) {}), std::runtime_error);
    EXPECT_TRUE(mutated);
    snapshot.reset();
    transaction->rollback();
    auto base = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(base->readNodeProperty(nodes[1], "payload"), PropertyValue("old"));
    EXPECT_EQ(base->readEdgeProperty(edges[1], "payload"), PropertyValue("old"));
    base.reset();
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, MultiKeyCellRetriesSkipEveryAlreadyRetainedValue) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(std::move(options)); backend.open();
    std::vector<LogicalGraphNodeCreateRequest> requests;
    for (size_t i = 0; i < 6U; ++i) requests.push_back({{"MultiCell"}, {
        {"first", PropertyValue(std::string(1200U, static_cast<char>('a' + i)))},
        {"second", PropertyValue(std::string(1200U, static_cast<char>('A' + i)))}}});
    const auto ids = backend.createLogicalNodes(requests);
    std::vector<LogicalGraphEdgeCreateRequest> edgeRequests;
    for (size_t i = 0; i < ids.size(); ++i) edgeRequests.push_back({ids[i], ids[i], "MULTI_CELL", requests[i].properties});
    ASSERT_EQ(backend.createLogicalEdges(edgeRequests).size(), ids.size());
    graph::parser::cypher::CypherParserImpl parser;
    execution::LogicalPlanReadRuntime runtime(backend);
    execution::LogicalPlanReadRuntimeBounds bounds{
        .maximum_node_records = 16U, .maximum_node_page_records = 4U,
        .maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 16U,
        .maximum_edge_records = 16U, .maximum_adjacency_page_records = 4U,
        .maximum_property_entries = 4U, .maximum_rows = 16U, .maximum_distinct_rows = 16U};
    for (const auto query : {"MATCH (n:MultiCell) RETURN n.first AS first, n.second AS second",
                            "MATCH ()-[e:MULTI_CELL]->() RETURN e.first AS first, e.second AS second"}) {
        const auto plan = parser.parseToLogical(query);
        for (size_t limit : {4096U, 6000U, 100000U}) {
            SCOPED_TRACE(query);
            SCOPED_TRACE(limit);
            bounds.maximum_materialized_bytes = limit;
            const bool oldEnabled = graph::debug::PerfTrace::areValuesEnabled();
            struct Restore { bool enabled; ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); graph::debug::PerfTrace::reset(); } } restore{oldEnabled};
            graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
            size_t peak = 0U;
            graph::debug::PerfTrace::ScopedObserver observer(&peak, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
                if (key == "logical_plan.materialized_budget_peak_bytes" && value > 0)
                    *static_cast<size_t *>(owner) = std::max(*static_cast<size_t *>(owner), static_cast<size_t>(value));
            });
            backend.resetIoStats();
            std::array<size_t, 6U> seen{};
            ASSERT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const execution::LogicalPlanReadRow &row) {
                const auto &first = std::get<std::string>(std::get<PropertyValue>(row.values.at("first")).getVariant());
                const auto &second = std::get<std::string>(std::get<PropertyValue>(row.values.at("second")).getVariant());
                EXPECT_EQ(first.size(), 1200U); EXPECT_EQ(second.size(), 1200U);
                if (!first.empty() && first.front() >= 'a' && first.front() <= 'f') {
                    ++seen[first.front() - 'a'];
                    EXPECT_EQ(first, std::string(1200U, first.front()));
                    EXPECT_EQ(second, std::string(1200U, static_cast<char>('A' + first.front() - 'a')));
                } else ADD_FAILURE() << "unexpected multi-key owner";
                return true;
            })));
            EXPECT_EQ(seen, (std::array<size_t, 6U>{1U, 1U, 1U, 1U, 1U, 1U}));
            const auto trace = graph::debug::PerfTrace::snapshotAndReset();
            ASSERT_TRUE(trace.contains("native_graph.read.selected_values_decoded"));
            EXPECT_EQ(trace.at("native_graph.read.selected_values_decoded").totalValue, 12);
            EXPECT_GT(peak, 0U); EXPECT_LE(peak, limit);
            std::fprintf(stderr, "MULTI_CELL limit=%zu peak=%zu data_read_bytes=%llu\n", limit, peak,
                static_cast<unsigned long long>(backend.ioStats().data.read_bytes));
        }
    }
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    backend.close();
}
