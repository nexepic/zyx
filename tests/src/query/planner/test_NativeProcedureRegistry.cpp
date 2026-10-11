/**
 * @file test_NativeProcedureRegistry.cpp
 * @brief Tests native logical procedure metadata without physical factories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <string>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/NativeProcedureManifest.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"

namespace graph::query::planner {

	TEST(NativeProcedureRegistryTest, ManifestRegistryAndRuntimeDescribeTheSameProcedureSurface) {
		ProcedureRegistry registry;
		const std::vector<std::string> names = registry.getRegisteredNames();
		std::vector<std::string> expected;
		expected.reserve(nativeProcedureManifest().size());
		for (const NativeProcedureManifestEntry &entry: nativeProcedureManifest()) {
			expected.emplace_back(entry.name);
			const ProcedureDescriptor *descriptor = registry.getDescriptor(std::string(entry.name));
			ASSERT_NE(descriptor, nullptr);
			EXPECT_EQ(descriptor->mutatesData, entry.mutates_data);
			EXPECT_EQ(descriptor->mutatesSchema, entry.mutates_schema);
			EXPECT_TRUE(execution::LogicalPlanProcedureRuntime::supportsNativeProcedure(entry.name));
		}

		std::ranges::sort(expected);
		EXPECT_EQ(names, expected);
		EXPECT_TRUE(execution::LogicalPlanProcedureRuntime::supportsNativeProcedure("dbms.setConfig"));
		EXPECT_FALSE(execution::LogicalPlanProcedureRuntime::supportsNativeProcedure("unknown.procedure"));
	}

	TEST(NativeProcedureRegistryTest, ExposesMutationMetadataForReadOnlyValidation) {
		ProcedureRegistry registry;
		const ProcedureDescriptor *shortestPath = registry.getDescriptor("algo.shortestPath");
		const ProcedureDescriptor *vectorQuery = registry.getDescriptor("db.index.vector.queryNodes");
		const ProcedureDescriptor *vectorTraining = registry.getDescriptor("db.index.vector.train");
		const ProcedureDescriptor *pageRank = registry.getDescriptor("gds.pageRank.stream");
		const ProcedureDescriptor *wcc = registry.getDescriptor("gds.wcc.stream");
		const ProcedureDescriptor *leiden = registry.getDescriptor("gds.leiden.stream");
		const ProcedureDescriptor *dijkstra = registry.getDescriptor("gds.shortestPath.dijkstra.stream");

		ASSERT_NE(shortestPath, nullptr);
		ASSERT_NE(vectorQuery, nullptr);
		ASSERT_NE(vectorTraining, nullptr);
		ASSERT_NE(pageRank, nullptr);
		ASSERT_NE(wcc, nullptr);
		ASSERT_NE(leiden, nullptr);
		ASSERT_NE(dijkstra, nullptr);
		EXPECT_FALSE(shortestPath->mutatesData);
		EXPECT_FALSE(shortestPath->mutatesSchema);
		EXPECT_FALSE(vectorQuery->mutatesData);
		EXPECT_FALSE(vectorQuery->mutatesSchema);
		EXPECT_TRUE(vectorTraining->mutatesData);
		EXPECT_FALSE(vectorTraining->mutatesSchema);
		EXPECT_FALSE(pageRank->mutatesData);
		EXPECT_FALSE(pageRank->mutatesSchema);
		EXPECT_FALSE(wcc->mutatesData);
		EXPECT_FALSE(wcc->mutatesSchema);
		EXPECT_FALSE(leiden->mutatesData);
		EXPECT_FALSE(leiden->mutatesSchema);
		EXPECT_FALSE(dijkstra->mutatesData);
		EXPECT_FALSE(dijkstra->mutatesSchema);
		const ProcedureDescriptor *setConfig = registry.getDescriptor("dbms.setConfig");
		const ProcedureDescriptor *showStats = registry.getDescriptor("dbms.showStats");
		ASSERT_NE(setConfig, nullptr);
		ASSERT_NE(showStats, nullptr);
		EXPECT_TRUE(setConfig->mutatesData);
		EXPECT_FALSE(setConfig->mutatesSchema);
		EXPECT_FALSE(showStats->mutatesData);
	}

	TEST(NativeProcedureRegistryTest, ClassifiesEveryNativeProcedureForExplicitTransactionExecution) {
		size_t overlaySnapshotReaders = 0U;
		size_t transactionLocalCatalogProcedures = 0U;
		size_t autocommitOnlyProcedures = 0U;
		for (const NativeProcedureManifestEntry &entry: nativeProcedureManifest()) {
			const NativeProcedureTransactionMode mode = nativeProcedureTransactionMode(entry.kind);
			switch (mode) {
			case NativeProcedureTransactionMode::NPTM_OVERLAY_SNAPSHOT_READ:
				++overlaySnapshotReaders;
				EXPECT_TRUE(entry.kind == NativeProcedureKind::NPK_SHORTEST_PATH ||
							entry.kind == NativeProcedureKind::NPK_VECTOR_QUERY_NODES);
				break;
			case NativeProcedureTransactionMode::NPTM_TRANSACTION_LOCAL_CATALOG:
				++transactionLocalCatalogProcedures;
				EXPECT_TRUE(entry.name.starts_with("gds."));
				break;
			case NativeProcedureTransactionMode::NPTM_AUTOCOMMIT_ONLY:
				++autocommitOnlyProcedures;
				EXPECT_TRUE(entry.name.starts_with("db.index.vector.") || entry.name.starts_with("dbms."));
				break;
			}
		}
		// Shortest-path and Vector search read a transaction-owned pinned snapshot;
		// GDS remains isolated in its transaction-local derived catalog.
		EXPECT_EQ(overlaySnapshotReaders, 2U);
		EXPECT_EQ(transactionLocalCatalogProcedures, 12U);
		EXPECT_EQ(autocommitOnlyProcedures, 8U);
	}

	TEST(NativeProcedureRegistryTest, RejectsInvalidAndDuplicateProcedureNames) {
		ProcedureRegistry registry;
		EXPECT_THROW(registry.registerProcedure({}, {}), std::invalid_argument);
		EXPECT_THROW(registry.registerProcedure("algo.shortestPath", {}), std::logic_error);

		registry.registerProcedure("test.custom", {.mutatesSchema = true});
		const ProcedureDescriptor *descriptor = registry.getDescriptor("test.custom");
		ASSERT_NE(descriptor, nullptr);
		EXPECT_FALSE(descriptor->mutatesData);
		EXPECT_TRUE(descriptor->mutatesSchema);
	}

} // namespace graph::query::planner
