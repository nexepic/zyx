/**
 * @file test_NativeProcedureRegistry.cpp
 * @brief Tests native logical procedure metadata without physical factories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/planner/ProcedureRegistry.hpp"

namespace graph::query::planner {

	TEST(NativeProcedureRegistryTest, RegistersExactlyTheProceduresSupportedByTheLogicalRuntime) {
		ProcedureRegistry registry;
		const std::vector<std::string> names = registry.getRegisteredNames();

		EXPECT_EQ(names, (std::vector<std::string>{"algo.shortestPath", "db.index.vector.queryNodes",
															 "db.index.vector.train"}));
	}

	TEST(NativeProcedureRegistryTest, ExposesMutationMetadataForReadOnlyValidation) {
		ProcedureRegistry registry;
		const ProcedureDescriptor *shortestPath = registry.getDescriptor("algo.shortestPath");
		const ProcedureDescriptor *vectorQuery = registry.getDescriptor("db.index.vector.queryNodes");
		const ProcedureDescriptor *vectorTraining = registry.getDescriptor("db.index.vector.train");

		ASSERT_NE(shortestPath, nullptr);
		ASSERT_NE(vectorQuery, nullptr);
		ASSERT_NE(vectorTraining, nullptr);
		EXPECT_FALSE(shortestPath->mutatesData);
		EXPECT_FALSE(shortestPath->mutatesSchema);
		EXPECT_FALSE(vectorQuery->mutatesData);
		EXPECT_FALSE(vectorQuery->mutatesSchema);
		EXPECT_TRUE(vectorTraining->mutatesData);
		EXPECT_FALSE(vectorTraining->mutatesSchema);
		EXPECT_EQ(registry.getDescriptor("dbms.setConfig"), nullptr);
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
