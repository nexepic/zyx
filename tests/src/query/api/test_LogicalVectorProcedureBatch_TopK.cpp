/**
 * @brief Vector procedure batching and deferred materialization contracts.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "query/api/LogicalVectorProcedureBatchFixture.hpp"
namespace graph::query {
	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKForSortSkipLimitPipeline) {
		options.bounds.maximum_rows = 1000U;
		for (uint64_t id = 4U; id <= 1000U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id * 10U))},
					{"tag", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id - 1U)});
		}
		std::vector<PropertyValue> queryCoordinates(500U, PropertyValue(1.0));
		const auto topKSearch = [&queryCoordinates] {
			return std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
					std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{1000}),
							PropertyValue(queryCoordinates)});
		};
		const size_t topKBudget = boundedTopKFixtureBytes() +
			queryCoordinates.size() * sizeof(float);
		options.bounds.maximum_materialized_bytes = topKBudget;
		backend->state->nodes.at(1U).properties.push_back({"tag", PropertyValue(int64_t{1})});
		backend->state->nodes.at(2U).properties.front().value = PropertyValue(int64_t{10});
		backend->state->nodes.at(2U).properties.push_back({"tag", PropertyValue(int64_t{2})});
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto tag = std::make_shared<expressions::VariableReferenceExpression>("node", "tag");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalSkip>(
								std::make_unique<logical::LogicalSort>(topKSearch(),
								std::vector<logical::LogicalSortItem>{{rank, true}}), 1), 1),
				{{tag, "tag"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("tag").asPrimitive(), PropertyValue(int64_t{2}));
        // Batches depend on STL capacities. Require bounded sorted gathers,
        // a selected-column-only path, and the exact final property fetch.
        ASSERT_FALSE(backend->state->property_column_ids.empty());
        EXPECT_GT(backend->state->property_column_reads, 1U);
        EXPECT_LE(backend->state->property_column_reads, 2U * 1000U + 1U);
        for (const auto &ids : backend->state->property_column_ids) {
            EXPECT_FALSE(ids.empty()); EXPECT_LE(ids.size(), 256U);
            EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
        }
		EXPECT_EQ(backend->state->property_column_ids.back(), (std::vector<uint64_t>{2U}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);

        // With the converted query still live, only one row frame remains;
        // omitting its charge would incorrectly admit the one-row heap.
        options.bounds.maximum_materialized_bytes =
            backend->state->search_result.hits.size() * sizeof(storage::LogicalGraphVectorSearchHit) +
            queryCoordinates.size() * sizeof(float) + sizeof(execution::LogicalPlanReadRow);
		EXPECT_THROW(static_cast<void>(execute(logical::LogicalLimit(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(topKSearch(),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
						std::vector<logical::LogicalProjectItem>{{tag, "tag"}}), 1))), std::runtime_error);

		backend->state->property_column_reads = 0U;
		backend->state->property_column_ids.clear();
		options.bounds.maximum_materialized_bytes = topKBudget;
		logical::LogicalLimit topDescending(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(search(1000),
								std::vector<logical::LogicalSortItem>{{rank, false}}),
						std::vector<logical::LogicalProjectItem>{{tag, "tag"}}), 1);
		const QueryResult descending = execute(topDescending);
		ASSERT_EQ(descending.rowCount(), 1U);
		EXPECT_EQ(descending.getRows().front().at("tag").asPrimitive(), PropertyValue(int64_t{1000}));
        // The same input cannot be fully sorted inside this byte ceiling;
        // successful limited output must retain a bounded heap.
        logical::LogicalSort fullSort(search(1000), {{rank, true}});
        EXPECT_THROW(static_cast<void>(execute(fullSort)), std::runtime_error);

	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKFilterReadsProjectedEntityAliasAfterItsBinding) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 1000U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 1000U;
		options.bounds.maximum_materialized_bytes = boundedTopKFixtureBytes();
		options.bounds.maximum_property_entries = 1U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto aliasedRank = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalProject output(
			std::make_unique<logical::LogicalLimit>(
				std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalSort>(search(1000),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
							std::vector<logical::LogicalProjectItem>{{
								std::make_shared<expressions::VariableReferenceExpression>("node"), "n"}}), predicate), 1), 2),
			{{aliasedRank, "rank"}});
		const QueryResult result = execute(output);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{8}));
		EXPECT_GT(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
        // The same input cannot be fully sorted inside this byte ceiling;
        // successful limited output must retain a bounded heap.
        logical::LogicalSort fullSort(search(1000), {{rank, true}});
        EXPECT_THROW(static_cast<void>(execute(fullSort)), std::runtime_error);

	}




	TEST_F(LogicalVectorPropertyBatchTest, DistinctBeforeLimitKeepsFullSortedCandidateSemantics) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 4U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSort>(search(4),
					std::vector<logical::LogicalSortItem>{{rank, true}}),
				std::vector<logical::LogicalProjectItem>{{rank, "r"}}, true), 2);
		const QueryResult result = execute(limit);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("r").asPrimitive(), PropertyValue(int64_t{1}));
		EXPECT_EQ(result.getRows()[1].at("r").asPrimitive(), PropertyValue(int64_t{2}));
	}




	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKWhenDistinctSortAndProjectionShareAnExpression) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 8U;
		// Only the two distinct rows needed by LIMIT may be retained. A full-sort
		// fallback would try to admit all four distinct projected values.
		options.bounds.maximum_distinct_rows = 2U;
		options.bounds.maximum_materialized_bytes = 8192U;
		for (uint64_t id = 1U; id <= 8U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rankPlusOne = [] {
			return std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_ADD,
				std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		};
		auto sorted = std::make_unique<logical::LogicalSort>(search(8),
			std::vector<logical::LogicalSortItem>{{rankPlusOne(), true}});
		auto distinct = std::make_unique<logical::LogicalProject>(std::move(sorted),
			std::vector<logical::LogicalProjectItem>{{rankPlusOne(), "rank"}}, true);
		logical::LogicalLimit limit(std::move(distinct), 2);

		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{2}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, KeepsFilterAfterSkipSemanticsInSortWindowPipeline) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalSkip>(
										std::make_unique<logical::LogicalSort>(search(10),
												std::vector<logical::LogicalSortItem>{{rank, true}}), 1), predicate), 2),
				{{rank, "rank"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{6}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
	}




	TEST_F(LogicalVectorPropertyBatchTest, BufferedSortPreservesStableTiesMultiKeyDirectionAndNulls) {
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue("shared");
		backend->state->nodes.at(2U).properties.front().value = PropertyValue(int64_t{10});
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(search(),
			std::vector<logical::LogicalSortItem>{{body, true}, {rank, false}}), {{score, "score"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		// Node 3 is first; equal rank Nodes 1/2 keep their search order.
		for (size_t index = 0U; index < 3U; ++index) {
			EXPECT_EQ(result.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(index)));
		}
		// Exceed small-array sort thresholds: equal NULL keys must retain the
		// full original order, not merely happen to survive a three-row sort.
		for (uint64_t id = 64U; id >= 4U; --id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(backend->state->search_result.hits.size())});
		}
		const auto missing = std::make_shared<expressions::VariableReferenceExpression>("node", "missing");
		logical::LogicalProject nullKeys(std::make_unique<logical::LogicalSort>(search(64),
			std::vector<logical::LogicalSortItem>{{missing, false}}), {{score, "score"}});
		const auto nullResult = execute(nullKeys);
		ASSERT_EQ(nullResult.rowCount(), 64U);
		for (size_t index = 0U; index < 64U; ++index) {
			EXPECT_EQ(nullResult.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(index)));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKComputedKeyReleasesUnreadSuffixAndPreservesWindowOrder) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		for (uint64_t id = 4U; id <= 20U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"body", PropertyValue(std::string(1000U, 'x'))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(backend->state->search_result.hits.size())});
		}
		std::unique_ptr<expressions::Expression> key = std::make_unique<expressions::VariableReferenceExpression>("node", "body");
		for (size_t repeat = 1U; repeat < 10U; ++repeat) {
			key = std::make_unique<expressions::BinaryOpExpression>(std::move(key), expressions::BinaryOperatorType::BOP_ADD,
				std::make_unique<expressions::VariableReferenceExpression>("node", "body"));
		}
		std::shared_ptr<expressions::Expression> sortKey = std::move(key);
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalSort>(search(20), std::vector<logical::LogicalSortItem>{{sortKey, true}}), 1),
			{{score, "score"}});
		// A computed 10 KiB key is larger than its selected 1 KiB source.
		// After the heap fills, keeping all unread prefetched rows would exceed
		// this allowance. Release their suffix before admitting the next key.
		options.bounds.maximum_materialized_bytes = 30000U;
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("score").asPrimitive(), PropertyValue(0.0));
		EXPECT_GT(backend->state->property_column_ids.size(), 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, BatchesFilterDistinctSkipLimitAcrossHitBoundaryAndStopsAtLimit) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 600U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			// Search order descends by node ID while retaining a valid increasing score.
			backend->state->search_result.hits.push_back({601U - id, static_cast<float>(id)});
		}
		// Repeat the first result's key on the first hit of batch two. DISTINCT
		// must retain its key set while SKIP and LIMIT retain their own counters.
		backend->state->nodes.at(344U).properties.front().value = PropertyValue(int64_t{600});
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{40}));
		logical::LogicalLimit limit(std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalFilter>(search(600), predicate),
						std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 257), 2);
		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{342}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{341}));
		EXPECT_EQ(backend->state->property_column_reads, 2U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 2U);
		EXPECT_EQ(backend->state->property_column_ids[0].size(), 256U);
		EXPECT_EQ(backend->state->property_column_ids[1].size(), 256U);
		for (const auto &ids: backend->state->property_column_ids) {
			EXPECT_LE(ids.size(), 256U);
			EXPECT_TRUE(std::ranges::is_sorted(ids));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKAllFilteredRowsStillChargesOutputColumnStorage) {
		for (uint64_t id = 4U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const std::string outputAlias(8192U, 't');
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto rejectAll = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{100}));
		auto project = std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalSkip>(
								std::make_unique<logical::LogicalFilter>(
										std::make_unique<logical::LogicalSort>(search(10),
												std::vector<logical::LogicalSortItem>{{rank, true}}), rejectAll), 0), 1),
				std::vector<logical::LogicalProjectItem>{{rank, outputAlias}});
		options.bounds.maximum_materialized_bytes = 4096U;
		EXPECT_THROW(static_cast<void>(execute(*project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 16U * 1024U;
		const QueryResult result = execute(*project);
		EXPECT_EQ(result.rowCount(), 0U);
		ASSERT_EQ(result.getColumns().size(), 1U);
		EXPECT_EQ(result.getColumns().front(), outputAlias);
	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKResultPublicationSharesHeapRowsAndRetainedResultBudget) {
		for (auto &[id, node]: backend->state->nodes) {
			static_cast<void>(id);
			node.properties.back().value = PropertyValue(std::string(1300U, 'x'));
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		logical::LogicalLimit topTwo(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(search(3),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
						std::vector<logical::LogicalProjectItem>{{body, "body"}}),
				2);
		options.bounds.maximum_materialized_bytes = 4000U;
		EXPECT_THROW(static_cast<void>(execute(topTwo)), std::runtime_error);

		options.bounds.maximum_materialized_bytes = 10U * 1024U;
		const QueryResult result = execute(topTwo);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("body").asPrimitive(), PropertyValue(std::string(1300U, 'x')));
		EXPECT_EQ(result.getRows()[1].at("body").asPrimitive(), PropertyValue(std::string(1300U, 'x')));
	}




	TEST_F(LogicalVectorPropertyBatchTest, UsesExpandableTopKWhenFilterFollowsSkipBeforeLimit) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 4096U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{4}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalSkip>(
										std::make_unique<logical::LogicalSort>(search(10),
											std::vector<logical::LogicalSortItem>{{rank, true}}), 1), predicate), 2),
					{{rank, "rank"}});

		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 8U * 1024U;
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{5}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{6}));
		EXPECT_GE(backend->state->property_column_reads, 3U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);

		backend->state->property_column_reads = 0U;
		const auto projectedPredicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{4}));
		logical::LogicalProject projectedFilter(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalProject>(
										std::make_unique<logical::LogicalSkip>(
												std::make_unique<logical::LogicalSort>(search(10),
													std::vector<logical::LogicalSortItem>{{rank, true}}), 1),
										std::vector<logical::LogicalProjectItem>{{rank, "r"}}), projectedPredicate), 2),
				{{std::make_shared<expressions::VariableReferenceExpression>("r"), "r"}});
		const QueryResult projectedResult = execute(projectedFilter);
		ASSERT_EQ(projectedResult.rowCount(), 2U);
		EXPECT_EQ(projectedResult.getRows()[0].at("r").asPrimitive(), PropertyValue(int64_t{5}));
		EXPECT_EQ(projectedResult.getRows()[1].at("r").asPrimitive(), PropertyValue(int64_t{6}));
		EXPECT_GE(backend->state->property_column_reads, 2U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKPrefetchesFilterPropertyThroughProjectedEntityAlias) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 1000U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))},
				{"selected", PropertyValue(id % 2U == 1U)}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 1000U;
		options.bounds.maximum_materialized_bytes = boundedTopKFixtureBytes();
		options.bounds.maximum_property_entries = 2U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::VariableReferenceExpression>("n", "selected");
		logical::LogicalProject output(
			std::make_unique<logical::LogicalLimit>(
				std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalSort>(search(1000),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
							std::vector<logical::LogicalProjectItem>{{
								std::make_shared<expressions::VariableReferenceExpression>("node"), "n"}}), predicate), 1), 2),
			{{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"}});
		const QueryResult result = execute(output);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{5}));
		ASSERT_FALSE(backend->state->property_column_keys.empty());
		for (const auto &keys: backend->state->property_column_keys) {
			EXPECT_EQ(keys, (std::vector<std::string>{"rank", "selected"}));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
        // The same input cannot be fully sorted inside this byte ceiling;
        // successful limited output must retain a bounded heap.
        logical::LogicalSort fullSort(search(1000), {{rank, true}});
        EXPECT_THROW(static_cast<void>(execute(fullSort)), std::runtime_error);

	}




	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKForSortDistinctSkipLimit) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 8U;
		options.bounds.maximum_distinct_rows = 3U;
		options.bounds.maximum_materialized_bytes = 8192U;
		for (uint64_t id = 1U; id <= 8U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalLimit projection(
			std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalProject>(
					std::make_unique<logical::LogicalSort>(search(8),
						std::vector<logical::LogicalSortItem>{{rank, true}}),
					std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 1), 2);

		const QueryResult result = execute(projection);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{2}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		logical::LogicalLimit descending(
			std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalProject>(
					std::make_unique<logical::LogicalSort>(search(8),
						std::vector<logical::LogicalSortItem>{{rank, false}}),
					std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 1), 2);
		const QueryResult descendingResult = execute(descending);
		ASSERT_EQ(descendingResult.rowCount(), 2U);
		EXPECT_EQ(descendingResult.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(descendingResult.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{2}));
		options.bounds.maximum_materialized_bytes = 1536U;
		EXPECT_THROW(static_cast<void>(execute(projection)), std::runtime_error);
	}




	TEST_F(LogicalVectorPropertyBatchTest, BufferedSortChargesHeapKeysTogetherWithRowsAndCanRetry) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, static_cast<char>('a' + id)));
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(search(),
			std::vector<logical::LogicalSortItem>{{body, true}}), {{rank, "rank"}});
		// Rows (~4.5 KiB) and owned string keys (~3 KiB) fit separately.
		// Their simultaneous retention must reject rather than use two allowances.
		options.bounds.maximum_materialized_bytes = 5000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 12000U;
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKHeapPayloadBackpressurePreservesStableTieWindowAcrossBatches) {
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		for (uint64_t id = 4U; id <= 40U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"body", PropertyValue(std::string(1000U, 'x'))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(backend->state->search_result.hits.size())});
		}
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalSort>(search(40), std::vector<logical::LogicalSortItem>{{body, true}}), 2),
			{{score, "score"}});
		options.bounds.maximum_materialized_bytes = 10000U;
		for (const bool admission: {false, true}) {
			backend->state->enable_property_admission = admission;
			backend->state->property_column_ids.clear();
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 2U);
			EXPECT_EQ(result.getRows()[0].at("score").asPrimitive(), PropertyValue(0.0));
			EXPECT_EQ(result.getRows()[1].at("score").asPrimitive(), PropertyValue(1.0));
			EXPECT_GT(backend->state->property_column_ids.size(), 1U);
			for (const auto &ids: backend->state->property_column_ids) EXPECT_TRUE(std::ranges::is_sorted(ids));
			EXPECT_EQ(backend->state->node_batch_reads, 0U);
		}
	}




	TEST_F(LogicalVectorPropertyBatchTest, CompleteBatchCacheSurvivesFilterSortAndProjection) {
		const auto countKeys = [] {
			std::vector<std::unique_ptr<expressions::Expression>> keysArguments;
			keysArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
			std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
			sizeArguments.push_back(std::make_unique<expressions::FunctionCallExpression>("keys", std::move(keysArguments)));
			return std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments));
		};
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(countKeys(),
			expressions::BinaryOperatorType::BOP_GREATER, std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		std::shared_ptr<expressions::Expression> sortKey = countKeys();
		std::vector<std::unique_ptr<expressions::Expression>> propertiesArguments;
		propertiesArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(propertiesArguments));
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(
			std::make_unique<logical::LogicalFilter>(search(), predicate),
			std::vector<logical::LogicalSortItem>{{sortKey, true}}), {{properties, "all"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("all").asPrimitive().getMap().at("rank"), PropertyValue(30));
		EXPECT_EQ(result.getRows()[1].at("all").asPrimitive().getMap().at("rank"), PropertyValue(10));
		EXPECT_EQ(result.getRows()[2].at("all").asPrimitive().getMap().at("rank"), PropertyValue(20));
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->property_column_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, ReusesMetadataAcrossEntityRenamingFilterAndSort) {
		backend->state->nodes.at(1U).labels = {"One", "Extra"};
		backend->state->nodes.at(2U).labels.clear();
		backend->state->nodes.at(3U).labels = {"Three"};
		const auto labels = [](const std::string &alias) {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>(alias));
			return std::make_unique<expressions::FunctionCallExpression>("labels", std::move(arguments));
		};
		const auto labelCount = [&](const std::string &alias) {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(labels(alias));
			return std::make_unique<expressions::FunctionCallExpression>("size", std::move(arguments));
		};
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(labelCount("entity"),
			expressions::BinaryOperatorType::BOP_GREATER, std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		std::shared_ptr<expressions::Expression> sortKey = labelCount("entity");
		std::shared_ptr<expressions::Expression> originalLabels = labels("node");
		std::shared_ptr<expressions::Expression> outputLabels = labels("entity");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(
			std::make_unique<logical::LogicalFilter>(std::make_unique<logical::LogicalProject>(search(),
				std::vector<logical::LogicalProjectItem>{{std::make_shared<expressions::VariableReferenceExpression>("node"), "entity"},
					{originalLabels, "initial"}}), predicate), std::vector<logical::LogicalSortItem>{{sortKey, true}}),
			{{outputLabels, "labels"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("labels").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue("Three")}));
		EXPECT_EQ(result.getRows()[1].at("labels").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue("One"), PropertyValue("Extra")}));
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->property_column_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, ReusesSortPropertiesForFollowingProjection) {
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{rank, true}}),
				{{rank, "rank"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("rank").asPrimitive(), PropertyValue(static_cast<int64_t>((index + 1U) * 10U)));
		}
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKWhenFilterPrecedesSkipAndLimitAfterSort) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 1000U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 1000U;
		options.bounds.maximum_materialized_bytes = boundedTopKFixtureBytes();
		options.bounds.maximum_property_entries = 1U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalSkip>(
								std::make_unique<logical::LogicalFilter>(
										std::make_unique<logical::LogicalSort>(search(1000),
												std::vector<logical::LogicalSortItem>{{rank, true}}), predicate), 1), 2),
				{{rank, "rank"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{8}));
		// Heap allocation peaks shrink read-ahead; exact ID preallocation
		// avoids geometric gather slack under this byte bound.
		EXPECT_GT(backend->state->property_column_reads, 1U);
        EXPECT_LE(backend->state->property_column_reads, 2U * 1000U + 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
        // The same input cannot be fully sorted inside this byte ceiling;
        // successful limited output must retain a bounded heap.
        logical::LogicalSort fullSort(search(1000), {{rank, true}});
        EXPECT_THROW(static_cast<void>(execute(fullSort)), std::runtime_error);

	}




	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKWhenFilterReadsAProjectedAliasBeforeWindow) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 2200U;
		options.bounds.maximum_property_entries = 1U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("r"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(search(10),
							std::vector<logical::LogicalSortItem>{{rank, true}}),
						std::vector<logical::LogicalProjectItem>{{rank, "r"}}), predicate), 1), 2);
		// The old and new heap buffers coexist during growth. The narrow
		// bound must reject that peak even when retained payload alone fits.
		EXPECT_THROW(static_cast<void>(execute(limit)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 4096U;
		backend->state->property_column_reads = 0U;
		const QueryResult result = execute(limit);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("r").asPrimitive(), PropertyValue(int64_t{7}));
		EXPECT_EQ(result.getRows()[1].at("r").asPrimitive(), PropertyValue(int64_t{8}));
		EXPECT_GT(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKPreservesFiltersOnBothSidesOfEntityProjection) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 1000U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))},
				{"selected", PropertyValue(id % 2U == 1U)}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 1000U;
		options.bounds.maximum_materialized_bytes = boundedTopKFixtureBytes();
		options.bounds.maximum_property_entries = 2U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto sourcePredicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{2}));
		const auto projectedPredicate = std::make_shared<expressions::VariableReferenceExpression>("n", "selected");
		logical::LogicalProject output(
			std::make_unique<logical::LogicalLimit>(
				std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalSort>(search(1000),
									std::vector<logical::LogicalSortItem>{{rank, true}}), sourcePredicate),
							std::vector<logical::LogicalProjectItem>{{
								std::make_shared<expressions::VariableReferenceExpression>("node"), "n"}}),
						projectedPredicate), 1), 2),
			{{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"}});
		const QueryResult result = execute(output);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{5}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
		ASSERT_FALSE(backend->state->property_column_keys.empty());
		for (const auto &keys: backend->state->property_column_keys) {
			EXPECT_EQ(keys, (std::vector<std::string>{"rank", "selected"}));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
        // The same input cannot be fully sorted inside this byte ceiling;
        // successful limited output must retain a bounded heap.
        logical::LogicalSort fullSort(search(1000), {{rank, true}});
        EXPECT_THROW(static_cast<void>(execute(fullSort)), std::runtime_error);

	}




	TEST_F(LogicalVectorPropertyBatchTest, DistinctTopKFallsBackWhenSortKeyIsNotProjected) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 4U;
		options.bounds.maximum_distinct_rows = 1U;
		for (uint64_t id = 1U; id <= 4U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSort>(search(4),
					std::vector<logical::LogicalSortItem>{{rank, true}}),
				std::vector<logical::LogicalProjectItem>{{
					std::make_shared<expressions::LiteralExpression>(int64_t{7}), "constant"}}, true), 1);

		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("constant").asPrimitive(), PropertyValue(int64_t{7}));
	}




	TEST_F(LogicalVectorPropertyBatchTest, ExpandsDistinctTopKForFilterAfterSkip) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_distinct_rows = 4U;
		options.bounds.maximum_materialized_bytes = 4096U;
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{2}));
		const auto secondPredicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("rank"),
			expressions::BinaryOperatorType::BOP_LESS,
			std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalFilter>(
				std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalSkip>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalSort>(search(10),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
							std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 1), predicate), secondPredicate), 2);

		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{4}));
	}




	TEST_F(LogicalVectorPropertyBatchTest, TopKHeapAndNextHeapPayloadShareAllowanceAndRetryCleanly) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, static_cast<char>('a' + id)));
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{body, true}}), 1),
			{{rank, "rank"}});
		// One retained row/key and the next row/key fit separately, but their
		// overlap exceeds 4 KiB even when only one row wins the final window.
		options.bounds.maximum_materialized_bytes = 4000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 12000U;
		backend->state->malformed_property_columns = true;
		const size_t reads = backend->state->property_column_reads;
		try { static_cast<void>(execute(project)); FAIL() << "malformed columns must fail"; }
		catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure backend changed Property batch cardinality");
		}
		EXPECT_EQ(backend->state->property_column_reads, reads + 1U);
		backend->state->malformed_property_columns = false;
		backend->state->before_property_admission = [] { throw std::domain_error("cancelled source"); };
		const size_t cancellationReads = backend->state->property_column_reads;
		EXPECT_THROW(static_cast<void>(execute(project)), std::domain_error);
		EXPECT_EQ(backend->state->property_column_reads, cancellationReads + 1U);
		backend->state->before_property_admission = {};
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}




	TEST_F(LogicalVectorPropertyBatchTest, PreservesDistinctProjectionStateAcrossHitBatches) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			const uint64_t rank = id == 44U ? 300U : id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(rank))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({301U - id, static_cast<float>(id - 1U)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(search(300), std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true);
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 299U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(int64_t{300}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{299}));
		EXPECT_EQ(std::ranges::count_if(result.getRows(), [](const QueryResult::Row &row) {
			return row.at("rank").asPrimitive() == PropertyValue(int64_t{300});
		}), 1);
		EXPECT_EQ(backend->state->property_column_reads, 2U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 2U);
		EXPECT_EQ(backend->state->property_column_ids.front().size(), 256U);
		EXPECT_EQ(backend->state->property_column_ids.back().size(), 44U);
		for (const auto &ids: backend->state->property_column_ids) {
			EXPECT_TRUE(std::ranges::is_sorted(ids));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}




} // namespace graph::query
