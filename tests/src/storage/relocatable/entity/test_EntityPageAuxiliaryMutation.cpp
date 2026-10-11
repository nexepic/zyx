#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/EntityPageAuxiliaryMutation.hpp"

namespace graph::storage {

TEST(EntityPageAuxiliaryMutationBatchTest, ReportsOnlyReachabilityChangingFramesAsNonEmpty) {
	EntityPageAuxiliaryMutationBatch batch;
	EXPECT_TRUE(batch.empty());

	// Bindings validate a frame mutation but cannot publish a change by themselves.
	batch.node_update_bindings.emplace_back();
	batch.node_insertion_bindings.emplace_back();
	EXPECT_TRUE(batch.empty());

	batch.replacements.emplace_back();
	EXPECT_FALSE(batch.empty());
	batch.replacements.clear();

	batch.appends.emplace_back();
	EXPECT_FALSE(batch.empty());
	batch.appends.clear();

	batch.new_pools.emplace_back();
	EXPECT_FALSE(batch.empty());
}

} // namespace graph::storage
