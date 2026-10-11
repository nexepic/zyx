/**
 * @file test_LogicalGraphReadExecutor_Validation.cpp
 * @brief Validation and defensive runtime tests for the logical graph reader.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalGraphReadExecutorTestFixture.hpp"

#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"

#include <cmath>
#include <limits>

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeFailsClosedForMalformedBoundsAndUnsupportedPlans) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n");
	LogicalPlanReadRuntimeBounds invalid = runtimeBounds();
	invalid.maximum_adjacency_page_records = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(scan, invalid, [](const LogicalPlanReadRow &) { return true; })),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), LogicalPlanReadRuntime::RowVisitor{})),
				 std::invalid_argument);

	logical::LogicalLimit negativeLimit(std::make_unique<logical::LogicalNodeScan>("n"), -1);
	EXPECT_THROW(static_cast<void>(runtime.execute(negativeLimit, runtimeBounds(),
												   [](const LogicalPlanReadRow &) { return true; })),
				 std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedReadOperatorContractsBeforeScanning) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto execute = [&runtime](const logical::LogicalOperator &root) {
		return runtime.execute(root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
	};
	const auto value = sharedExpression(std::make_unique<expressions::LiteralExpression>(int64_t{1}));

	// These direct logical-plan shapes are public runtime inputs. Reject them
	// before opening a scan so malformed planner output cannot become execution.
	logical::LogicalNodeScan emptyScanVariable("");
	EXPECT_THROW(static_cast<void>(execute(emptyScanVariable)), std::invalid_argument);
	logical::LogicalAggregate noItems(std::make_unique<logical::LogicalSingleRow>(), {}, {});
	EXPECT_THROW(static_cast<void>(execute(noItems)), std::invalid_argument);
	logical::LogicalAggregate excessAliases(
			std::make_unique<logical::LogicalSingleRow>(), {value}, {{"count", nullptr, "total", false}}, {"group", "extra"});
	EXPECT_THROW(static_cast<void>(execute(excessAliases)), std::invalid_argument);
	logical::LogicalAggregate missingGroup(std::make_unique<logical::LogicalSingleRow>(), {nullptr},
			{{"count", nullptr, "total", false}});
	EXPECT_THROW(static_cast<void>(execute(missingGroup)), std::invalid_argument);
	logical::LogicalAggregate emptyAlias(std::make_unique<logical::LogicalSingleRow>(), {}, {{"sum", value, "", false}});
	EXPECT_THROW(static_cast<void>(execute(emptyAlias)), std::invalid_argument);
	logical::LogicalAggregate duplicateAlias(std::make_unique<logical::LogicalSingleRow>(), {},
			{{"sum", value, "total", false}, {"avg", value, "total", false}});
	EXPECT_THROW(static_cast<void>(execute(duplicateAlias)), std::invalid_argument);
	logical::LogicalAggregate missingArgument(std::make_unique<logical::LogicalSingleRow>(), {}, {{"sum", nullptr, "total", false}});
	EXPECT_THROW(static_cast<void>(execute(missingArgument)), std::invalid_argument);
	logical::LogicalAggregate distinctCountStar(std::make_unique<logical::LogicalSingleRow>(), {}, {{"count", nullptr, "total", true}});
	EXPECT_THROW(static_cast<void>(execute(distinctCountStar)), std::invalid_argument);

	logical::LogicalJoin missingLeft(nullptr, std::make_unique<logical::LogicalSingleRow>());
	EXPECT_THROW(static_cast<void>(execute(missingLeft)), std::invalid_argument);
	logical::LogicalJoin missingRight(std::make_unique<logical::LogicalSingleRow>(), nullptr);
	EXPECT_THROW(static_cast<void>(execute(missingRight)), std::invalid_argument);
	auto duplicateColumns = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{value, "duplicate"}, {value, "duplicate"}});
	logical::LogicalJoin duplicateJoin(std::move(duplicateColumns), std::make_unique<logical::LogicalSingleRow>());
	EXPECT_THROW(static_cast<void>(execute(duplicateJoin)), std::invalid_argument);

	logical::LogicalOptionalMatch missingPattern(std::make_unique<logical::LogicalSingleRow>(), nullptr);
	EXPECT_THROW(static_cast<void>(execute(missingPattern)), std::invalid_argument);
	logical::LogicalOptionalMatch missingInput(nullptr, std::make_unique<logical::LogicalSingleRow>());
	EXPECT_THROW(static_cast<void>(execute(missingInput)), std::invalid_argument);
	auto emptyInputColumn = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(), std::vector<logical::LogicalProjectItem>{{value, ""}});
	logical::LogicalOptionalMatch invalidInputColumn(std::move(emptyInputColumn),
			std::make_unique<logical::LogicalSingleRow>());
	EXPECT_THROW(static_cast<void>(execute(invalidInputColumn)), std::invalid_argument);
	auto duplicatePatternColumn = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{value, "duplicate"}, {value, "duplicate"}});
	logical::LogicalOptionalMatch invalidPatternColumn(std::make_unique<logical::LogicalSingleRow>(),
			std::move(duplicatePatternColumn));
	EXPECT_THROW(static_cast<void>(execute(invalidPatternColumn)), std::invalid_argument);
	logical::LogicalOptionalMatch unknownRequired(std::make_unique<logical::LogicalSingleRow>(),
			std::make_unique<logical::LogicalSingleRow>(), {"missing"});
	EXPECT_THROW(static_cast<void>(execute(unknownRequired)), std::invalid_argument);
	logical::LogicalOptionalMatch emptyRequired(std::make_unique<logical::LogicalSingleRow>(),
			std::make_unique<logical::LogicalSingleRow>(), {""});
	EXPECT_THROW(static_cast<void>(execute(emptyRequired)), std::invalid_argument);

	logical::LogicalCallSubquery missingBody(nullptr, nullptr, {}, {});
	EXPECT_THROW(static_cast<void>(execute(missingBody)), std::invalid_argument);
	logical::LogicalCallSubquery transactional(nullptr, std::make_unique<logical::LogicalSingleRow>(), {}, {}, true);
	EXPECT_THROW(static_cast<void>(execute(transactional)), std::invalid_argument);
	logical::LogicalCallSubquery explicitBatch(nullptr, std::make_unique<logical::LogicalSingleRow>(), {}, {}, false, 1);
	EXPECT_THROW(static_cast<void>(execute(explicitBatch)), std::invalid_argument);
	logical::LogicalCallSubquery duplicateReturned(nullptr, std::make_unique<logical::LogicalSingleRow>(), {}, {"value", "value"});
	EXPECT_THROW(static_cast<void>(execute(duplicateReturned)), std::invalid_argument);
	logical::LogicalCallSubquery emptyReturned(nullptr, std::make_unique<logical::LogicalSingleRow>(), {}, {""});
	EXPECT_THROW(static_cast<void>(execute(emptyReturned)), std::invalid_argument);
	logical::LogicalCallSubquery emptyImported(nullptr, std::make_unique<logical::LogicalSingleRow>(), {""}, {});
	EXPECT_THROW(static_cast<void>(execute(emptyImported)), std::invalid_argument);
	auto importedValue = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(), std::vector<logical::LogicalProjectItem>{{value, "value"}});
	logical::LogicalCallSubquery duplicateImported(std::move(importedValue),
			std::make_unique<logical::LogicalSingleRow>(), {"value", "value"}, {});
	EXPECT_THROW(static_cast<void>(execute(duplicateImported)), std::runtime_error);

	logical::LogicalNamedPath emptyPath(std::make_unique<logical::LogicalSingleRow>(), "", {"start", "end"}, {"edge"});
	EXPECT_THROW(static_cast<void>(execute(emptyPath)), std::invalid_argument);
	logical::LogicalNamedPath noNodes(std::make_unique<logical::LogicalSingleRow>(), "path", {}, {});
	EXPECT_THROW(static_cast<void>(execute(noNodes)), std::invalid_argument);
	logical::LogicalNamedPath mismatchedPath(std::make_unique<logical::LogicalSingleRow>(), "path", {"start", "middle", "end"}, {"edge"});
	EXPECT_THROW(static_cast<void>(execute(mismatchedPath)), std::invalid_argument);
	logical::LogicalNamedPath anonymousNode(std::make_unique<logical::LogicalSingleRow>(), "path", {"", "end"}, {"edge"});
	EXPECT_THROW(static_cast<void>(execute(anonymousNode)), std::invalid_argument);
	logical::LogicalNamedPath anonymousEdge(std::make_unique<logical::LogicalSingleRow>(), "path", {"start", "end"}, {""});
	EXPECT_THROW(static_cast<void>(execute(anonymousEdge)), std::invalid_argument);

	// UNION and Sort are public logical-plan boundaries too. These malformed
	// shapes must fail before they can execute either input pipeline.
	logical::LogicalUnion missingUnionLeft(nullptr, std::make_unique<logical::LogicalSingleRow>(), false);
	EXPECT_THROW(static_cast<void>(execute(missingUnionLeft)), std::invalid_argument);
	logical::LogicalUnion missingUnionRight(std::make_unique<logical::LogicalSingleRow>(), nullptr, false);
	EXPECT_THROW(static_cast<void>(execute(missingUnionRight)), std::invalid_argument);
	logical::LogicalUnion emptyUnionColumns(std::make_unique<logical::LogicalSingleRow>(),
			std::make_unique<logical::LogicalSingleRow>(), false);
	EXPECT_THROW(static_cast<void>(execute(emptyUnionColumns)), std::invalid_argument);
	const auto leftValue = sharedExpression(std::make_unique<expressions::LiteralExpression>(int64_t{1}));
	const auto rightValue = sharedExpression(std::make_unique<expressions::LiteralExpression>(int64_t{2}));
	auto leftProjection = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{leftValue, "left"}});
	auto rightProjection = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{rightValue, "right"}});
	logical::LogicalUnion mismatchedUnionColumns(std::move(leftProjection), std::move(rightProjection), false);
	EXPECT_THROW(static_cast<void>(execute(mismatchedUnionColumns)), std::invalid_argument);
	auto duplicateLeft = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{leftValue, "duplicate"}, {leftValue, "duplicate"}});
	auto duplicateRight = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{rightValue, "duplicate"}, {rightValue, "duplicate"}});
	logical::LogicalUnion duplicateUnionColumns(std::move(duplicateLeft), std::move(duplicateRight), false);
	EXPECT_THROW(static_cast<void>(execute(duplicateUnionColumns)), std::invalid_argument);
	logical::LogicalSort emptySortItems(std::make_unique<logical::LogicalSingleRow>(), {});
	EXPECT_THROW(static_cast<void>(execute(emptySortItems)), std::invalid_argument);
	logical::LogicalSort nullSortExpression(std::make_unique<logical::LogicalSingleRow>(), {{nullptr, true}});
	EXPECT_THROW(static_cast<void>(execute(nullSortExpression)), std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsProjectionOnlyItemsInMapLiterals) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);

	const auto expectRejected = [&runtime](expressions::MapProjectionItemType itemType, std::string key) {
		std::vector<expressions::MapProjectionItem> mapItems;
		mapItems.emplace_back(itemType, std::move(key));
		std::vector<logical::LogicalProjectItem> projectItems;
		projectItems.emplace_back(std::make_shared<expressions::MapProjectionExpression>(std::move(mapItems)), "map");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(projectItems));
		EXPECT_THROW(static_cast<void>(runtime.execute(
					 root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
				 std::logic_error);
	};

	expectRejected(expressions::MapProjectionItemType::MPROP_PROPERTY, "property");
	expectRejected(expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	expectRejected(expressions::MapProjectionItemType::MPROP_LITERAL, "");
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeProjectsAllPropertiesFromMapBindings) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const PropertyValue source(PropertyValue::MapType{{"answer", PropertyValue(42)}, {"name", PropertyValue("Zyx")}});

	std::vector<logical::LogicalProjectItem> sourceItems;
	sourceItems.emplace_back(std::make_shared<expressions::LiteralExpression>(source), "map");
	auto sourceProject = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(), std::move(sourceItems));

	std::vector<expressions::MapProjectionItem> projectionItems;
	projectionItems.emplace_back(expressions::MapProjectionItemType::MPROP_PROPERTY, "answer");
	projectionItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "computed",
			std::make_unique<expressions::LiteralExpression>(int64_t{7}));
	projectionItems.emplace_back(expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	std::vector<logical::LogicalProjectItem> resultItems;
	resultItems.emplace_back(std::make_shared<expressions::MapProjectionExpression>("map", std::move(projectionItems)),
						 "result");
	logical::LogicalProject root(std::move(sourceProject), std::move(resultItems));

	std::vector<PropertyValue> results;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(root, runtimeBounds(), [&results](const LogicalPlanReadRow &row) {
		results.push_back(std::get<PropertyValue>(row.values.at("result")));
		return true;
	});

	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
	ASSERT_EQ(results.size(), 1U);
	EXPECT_EQ(results.front(),
			  PropertyValue(PropertyValue::MapType{{"answer", PropertyValue(42)}, {"computed", PropertyValue(7)},
															 {"name", PropertyValue("Zyx")}}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeValidatesMapProjectionBoundaryContracts) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto visitor = [](const LogicalPlanReadRow &) { return true; };

	// A projection always needs a source binding; only a map literal may omit it.
	logical::LogicalProject missingSource(std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{std::make_shared<expressions::MapProjectionExpression>(
						"", std::vector<expressions::MapProjectionItem>{}),
					"result"}});
	EXPECT_THROW(static_cast<void>(runtime.execute(missingSource, runtimeBounds(), visitor)), std::logic_error);

	std::vector<logical::LogicalProjectItem> sourceItems;
	sourceItems.emplace_back(std::make_shared<expressions::LiteralExpression>(
				PropertyValue(PropertyValue::MapType{{"present", PropertyValue(int64_t{1})}})),
			"map");
	std::vector<expressions::MapProjectionItem> emptyPropertyItems;
	emptyPropertyItems.emplace_back(expressions::MapProjectionItemType::MPROP_PROPERTY, "");
	std::vector<logical::LogicalProjectItem> emptyPropertyResult;
	emptyPropertyResult.emplace_back(
			std::make_shared<expressions::MapProjectionExpression>("map", std::move(emptyPropertyItems)), "result");
	logical::LogicalProject emptyProperty(std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSingleRow>(), std::move(sourceItems)),
			std::move(emptyPropertyResult));
	EXPECT_THROW(static_cast<void>(runtime.execute(emptyProperty, runtimeBounds(), visitor)), std::logic_error);

	std::vector<logical::LogicalProjectItem> scalarSourceItems;
	scalarSourceItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{7}), "scalar");
	std::vector<logical::LogicalProjectItem> scalarProjectionItems;
	scalarProjectionItems.emplace_back(
			std::make_shared<expressions::MapProjectionExpression>("scalar", std::vector<expressions::MapProjectionItem>{}),
			"result");
	logical::LogicalProject scalarProjection(
			std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(scalarSourceItems)),
			std::move(scalarProjectionItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(scalarProjection, runtimeBounds(), visitor)), std::runtime_error);

	std::vector<logical::LogicalProjectItem> nullSourceItems;
	nullSourceItems.emplace_back(std::make_shared<expressions::LiteralExpression>(PropertyValue()), "null_map");
	std::vector<logical::LogicalProjectItem> nullProjectionItems;
	nullProjectionItems.emplace_back(
			std::make_shared<expressions::MapProjectionExpression>("null_map", std::vector<expressions::MapProjectionItem>{}),
			"result");
	logical::LogicalProject nullProjection(
			std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(nullSourceItems)),
			std::move(nullProjectionItems));
	std::vector<PropertyValue> nullProjectionValues;
	static_cast<void>(runtime.execute(nullProjection, runtimeBounds(), [&nullProjectionValues](const LogicalPlanReadRow &row) {
		nullProjectionValues.push_back(std::get<PropertyValue>(row.values.at("result")));
		return true;
	}));
	ASSERT_EQ(nullProjectionValues.size(), 1U);
	EXPECT_EQ(nullProjectionValues.front(), PropertyValue());

	std::vector<expressions::MapProjectionItem> nullLiteralItems;
	nullLiteralItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "value");
	std::vector<logical::LogicalProjectItem> nullLiteralResult;
	nullLiteralResult.emplace_back(
			std::make_shared<expressions::MapProjectionExpression>(std::move(nullLiteralItems)), "result");
	logical::LogicalProject nullLiteral(std::make_unique<logical::LogicalSingleRow>(), std::move(nullLiteralResult));
	std::vector<PropertyValue> values;
	static_cast<void>(runtime.execute(nullLiteral, runtimeBounds(), [&values](const LogicalPlanReadRow &row) {
		values.push_back(std::get<PropertyValue>(row.values.at("result")));
		return true;
	}));
	ASSERT_EQ(values.size(), 1U);
	EXPECT_EQ(values.front(), PropertyValue(PropertyValue::MapType{{"value", PropertyValue()}}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeUsesFallbackAggregateAliasesAndHonorsOutputCancellation) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto groupValue = sharedExpression(std::make_unique<expressions::LiteralExpression>(int64_t{7}));
	const auto countStar = logical::LogicalAggItem{"count", nullptr, "count", false};

	logical::LogicalAggregate fallbackAlias(std::make_unique<logical::LogicalSingleRow>(), {groupValue}, {countStar});
	std::vector<LogicalPlanReadRow> rows;
	static_cast<void>(runtime.execute(fallbackAlias, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
		rows.push_back(row);
		return true;
	}));
	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("7")), PropertyValue(int64_t{7}));
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("count")), PropertyValue(int64_t{1}));

	logical::LogicalAggregate cancelledOutput(std::make_unique<logical::LogicalSingleRow>(), {groupValue}, {countStar});
	size_t callbackCount = 0U;
	static_cast<void>(runtime.execute(cancelledOutput, runtimeBounds(), [&callbackCount](const LogicalPlanReadRow &) {
		++callbackCount;
		return false;
	}));
	EXPECT_EQ(callbackCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeHandlesLegacyExistsAndRejectsIncompletePatternComprehensions) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto visitor = [](const LogicalPlanReadRow &) { return true; };

	std::vector<logical::LogicalProjectItem> legacyExistsItems;
	legacyExistsItems.emplace_back(std::make_shared<expressions::ExistsExpression>("(n)-[:REL]->(m)"), "exists");
	logical::LogicalProject legacyExists(std::make_unique<logical::LogicalSingleRow>(), std::move(legacyExistsItems));
	PropertyValue existsValue;
	static_cast<void>(runtime.execute(legacyExists, runtimeBounds(), [&existsValue](const LogicalPlanReadRow &row) {
		existsValue = std::get<PropertyValue>(row.values.at("exists"));
		return true;
	}));
	EXPECT_EQ(existsValue, PropertyValue(false));

	std::vector<logical::LogicalProjectItem> emptySourceItems;
	emptySourceItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)", "", std::make_unique<expressions::LiteralExpression>(int64_t{1})),
			"values");
	logical::LogicalProject emptySource(std::make_unique<logical::LogicalSingleRow>(), std::move(emptySourceItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(emptySource, runtimeBounds(), visitor)), std::logic_error);

	std::vector<logical::LogicalProjectItem> missingMapItems;
	missingMapItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)", "n", "", "", "", expressions::PatternDirection::PAT_OUTGOING, nullptr),
			"values");
	logical::LogicalProject missingMap(std::make_unique<logical::LogicalSingleRow>(), std::move(missingMapItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(missingMap, runtimeBounds(), visitor)), std::logic_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeOmitsTheDuplicateSelfLoopFromBothPatternComprehensions) {
	LogicalGraphFixtureData data = graphData();
	data.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 1U, "LOOP"});
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_edges_per_node = 5U;

	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(
			sharedExpression(std::make_unique<expressions::PatternComprehensionExpression>(
					"(source)-[relationship]-(target)", "source", "target", "", "",
					expressions::PatternDirection::PAT_BOTH,
					std::make_unique<expressions::VariableReferenceExpression>("target", "age"), nullptr,
					"relationship")),
			"ages");
	logical::LogicalProject root(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}), std::move(items));

	std::vector<PropertyValue> ages;
	static_cast<void>(runtime.execute(root, bounds, [&ages](const LogicalPlanReadRow &row) {
		ages = std::get<PropertyValue>(row.values.at("ages")).getList();
		return true;
	}));
	// The LOOP Edge is returned by both directions but contributes its source only once.
	EXPECT_EQ(ages, (std::vector<PropertyValue>{PropertyValue(30), PropertyValue(), PropertyValue(42), PropertyValue(30)}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsEntityFunctionsOnNonEntityBindings) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto visitor = [](const LogicalPlanReadRow &) { return true; };

	const auto scalarInput = [] {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(7)), "scalar");
		return std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	};
	const auto entityFunction = [](std::string name, std::vector<std::unique_ptr<expressions::Expression>> arguments) {
		return sharedExpression(std::make_unique<expressions::FunctionCallExpression>(std::move(name),
																					 std::move(arguments)));
	};
	const auto execute = [&](std::string name, std::vector<std::unique_ptr<expressions::Expression>> arguments) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(entityFunction(std::move(name), std::move(arguments)), "result");
		logical::LogicalProject root(scalarInput(), std::move(items));
		return runtime.execute(root, runtimeBounds(), visitor);
	};

	EXPECT_THROW(static_cast<void>(execute("id", {})), std::runtime_error);
	std::vector<std::unique_ptr<expressions::Expression>> nullArgument(1U);
	EXPECT_THROW(static_cast<void>(execute("id", std::move(nullArgument))), std::runtime_error);
	std::vector<std::unique_ptr<expressions::Expression>> literalArgument;
	literalArgument.push_back(std::make_unique<expressions::LiteralExpression>(int64_t(7)));
	EXPECT_THROW(static_cast<void>(execute("id", std::move(literalArgument))), std::runtime_error);
	std::vector<std::unique_ptr<expressions::Expression>> propertyArgument;
	propertyArgument.push_back(std::make_unique<expressions::VariableReferenceExpression>("scalar", "property"));
	EXPECT_THROW(static_cast<void>(execute("id", std::move(propertyArgument))), std::runtime_error);
	for (const std::string &name: {"id", "labels", "type", "keys", "properties"}) {
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("scalar"));
		EXPECT_THROW(static_cast<void>(execute(name, std::move(arguments))), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBoundsStaticAndDynamicCollectionExpressions) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds collectionBounds = runtimeBounds();
	collectionBounds.maximum_rows = 2U;
	collectionBounds.maximum_property_entries = 1U;

	const auto expectRejected = [&runtime, &collectionBounds](std::shared_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> projectItems;
		projectItems.emplace_back(std::move(expression), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(projectItems));
		EXPECT_THROW(static_cast<void>(runtime.execute(
					 root, collectionBounds, [](const LogicalPlanReadRow &) { return true; })),
				 std::runtime_error);
	};

	expectRejected(std::make_shared<expressions::ListLiteralExpression>(
				PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)})));

	std::vector<std::unique_ptr<expressions::Expression>> dynamicList;
	for (int64_t value = 1; value <= 3; ++value) {
		dynamicList.push_back(std::make_unique<expressions::LiteralExpression>(value));
	}
	expectRejected(std::make_shared<expressions::ListLiteralExpression>(std::move(dynamicList)));

	std::vector<expressions::MapProjectionItem> mapItems;
	mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "first",
			std::make_unique<expressions::LiteralExpression>(int64_t(1)));
	mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "second",
			std::make_unique<expressions::LiteralExpression>(int64_t(2)));
	expectRejected(std::make_shared<expressions::MapProjectionExpression>(std::move(mapItems)));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeEvaluatesDynamicListLiteralsWithinTheConfiguredBound) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	std::vector<std::unique_ptr<expressions::Expression>> elements;
	elements.push_back(std::make_unique<expressions::LiteralExpression>(int64_t{7}));
	elements.push_back(std::make_unique<expressions::LiteralExpression>(std::string("eight")));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::ListLiteralExpression>(std::move(elements)), "values");
	logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));

	std::vector<LogicalPlanReadRow> rows;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(root, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
		rows.push_back(row);
		return true;
	});
	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("values")).getList(),
			  (std::vector<PropertyValue>{PropertyValue(7), PropertyValue("eight")}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRestoresAnOuterMapBindingAfterAListComprehension) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalPlanReadRow row;
	const PropertyValue outerMap(PropertyValue::MapType{{"stable", PropertyValue(42)}});
	row.values.emplace("item", outerMap);
	graph::query::QueryContext context;
	const auto evaluateProperties = [&] {
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("item"));
		expressions::FunctionCallExpression properties("properties", std::move(arguments));
		return LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(*snapshot, properties, row, runtimeBounds(), context);
	};

	EXPECT_EQ(evaluateProperties(), LogicalPlanReadValue(outerMap));
	ASSERT_TRUE(row.property_cache.contains("item"));
	expressions::ListComprehensionExpression comprehension(
			"item",
			std::make_unique<expressions::ListLiteralExpression>(
					PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})),
			nullptr, nullptr, expressions::ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
				  *snapshot, comprehension, row, runtimeBounds(), context))
				  .getList(),
			  (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
	EXPECT_EQ(std::get<PropertyValue>(row.values.at("item")), outerMap);
	EXPECT_EQ(evaluateProperties(), LogicalPlanReadValue(outerMap));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsIncompleteCollectionExpressionContracts) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto expectRejected = [&runtime](std::shared_ptr<expressions::Expression> expression, bool runtimeError = false) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(expression), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		if (runtimeError) {
			EXPECT_THROW(static_cast<void>(runtime.execute(
					 root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
		} else {
			EXPECT_THROW(static_cast<void>(runtime.execute(
					 root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })), std::logic_error);
		}
	};

	expectRejected(std::make_shared<expressions::ListSliceExpression>(nullptr, nullptr, nullptr, false));
	expectRejected(std::make_shared<expressions::ListComprehensionExpression>(
				"", nullptr, nullptr, nullptr, expressions::ListComprehensionExpression::ComprehensionType::COMP_FILTER));
	expectRejected(std::make_shared<expressions::ListComprehensionExpression>(
				"item", std::make_unique<expressions::ListLiteralExpression>(
							PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})),
				nullptr, nullptr, expressions::ListComprehensionExpression::ComprehensionType::COMP_REDUCE));
	expectRejected(std::make_shared<expressions::ReduceExpression>(
				"", std::make_unique<expressions::LiteralExpression>(int64_t(0)), "item",
				std::make_unique<expressions::ListLiteralExpression>(
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})),
				std::make_unique<expressions::LiteralExpression>(int64_t(1))));
	expectRejected(std::make_shared<expressions::QuantifierFunctionExpression>(
				"unsupported", "item", std::make_unique<expressions::ListLiteralExpression>(
										PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})),
				std::make_unique<expressions::LiteralExpression>(true)),
				true);
	expectRejected(std::make_shared<expressions::QuantifierFunctionExpression>("any", "", nullptr, nullptr));

	auto incompleteCase = std::make_shared<expressions::CaseExpression>();
	incompleteCase->addBranch(nullptr, std::make_unique<expressions::LiteralExpression>(int64_t(1)));
	expectRejected(std::move(incompleteCase));
	auto incompleteSimpleCase =
			std::make_shared<expressions::CaseExpression>(std::make_unique<expressions::LiteralExpression>(int64_t(1)));
	incompleteSimpleCase->addBranch(nullptr, std::make_unique<expressions::LiteralExpression>(int64_t(1)));
	expectRejected(std::move(incompleteSimpleCase));

	const auto list = [] {
		return std::make_unique<expressions::ListLiteralExpression>(
				PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{1})}));
	};
	// Each public expression constructor permits deferred planner assembly. The
	// runtime must identify the missing member precisely when execution starts.
	expectRejected(std::make_shared<expressions::ListSliceExpression>(
			list(), nullptr, nullptr, false));
	expectRejected(std::make_shared<expressions::ListSliceExpression>(
			list(), std::make_unique<expressions::LiteralExpression>(std::string("not-an-index")), nullptr, false), true);
	expectRejected(std::make_shared<expressions::ListComprehensionExpression>(
			"item", nullptr, nullptr, nullptr, expressions::ListComprehensionExpression::ComprehensionType::COMP_FILTER));
	expectRejected(std::make_shared<expressions::ListComprehensionExpression>(
			"", list(), nullptr, nullptr, expressions::ListComprehensionExpression::ComprehensionType::COMP_FILTER));
	expectRejected(std::make_shared<expressions::ReduceExpression>(
			"total", nullptr, "item", list(), std::make_unique<expressions::LiteralExpression>(int64_t{1})));
	expectRejected(std::make_shared<expressions::ReduceExpression>(
			"total", std::make_unique<expressions::LiteralExpression>(int64_t{0}), "item", nullptr,
			std::make_unique<expressions::LiteralExpression>(int64_t{1})));
	expectRejected(std::make_shared<expressions::ReduceExpression>(
			"total", std::make_unique<expressions::LiteralExpression>(int64_t{0}), "item", list(), nullptr));
	expectRejected(std::make_shared<expressions::ReduceExpression>(
			"", std::make_unique<expressions::LiteralExpression>(int64_t{0}), "item", list(),
			std::make_unique<expressions::LiteralExpression>(int64_t{1})));
	expectRejected(std::make_shared<expressions::ReduceExpression>(
			"total", std::make_unique<expressions::LiteralExpression>(int64_t{0}), "", list(),
			std::make_unique<expressions::LiteralExpression>(int64_t{1})));
	expectRejected(std::make_shared<expressions::QuantifierFunctionExpression>(
			"any", "item", nullptr, std::make_unique<expressions::LiteralExpression>(true)));
	expectRejected(std::make_shared<expressions::QuantifierFunctionExpression>(
			"any", "item", list(), nullptr));
	expectRejected(std::make_shared<expressions::QuantifierFunctionExpression>(
			"any", "", list(), std::make_unique<expressions::LiteralExpression>(true)));
	auto missingThenCase = std::make_shared<expressions::CaseExpression>();
	missingThenCase->addBranch(std::make_unique<expressions::LiteralExpression>(true), nullptr);
	expectRejected(std::move(missingThenCase));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCoversScalarCollectionBoundarySemantics) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);

	const auto literal = [](PropertyValue value) {
		return std::make_unique<expressions::LiteralExpression>(std::move(value));
	};
	const auto arguments = []<typename... T>(T... expressions) {
		std::vector<std::unique_ptr<expressions::Expression>> result;
		result.reserve(sizeof...(expressions));
		(result.push_back(std::move(expressions)), ...);
		return result;
	};
	const auto evaluateFunction = [&runtime](std::string name,
			std::vector<std::unique_ptr<expressions::Expression>> arguments) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>(std::move(name), std::move(arguments)),
				"value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		PropertyValue result;
		static_cast<void>(runtime.execute(root, runtimeBounds(), [&result](const LogicalPlanReadRow &row) {
			result = std::get<PropertyValue>(row.values.at("value"));
			return true;
		}));
		return result;
	};

	EXPECT_EQ(evaluateFunction("range", arguments(literal(PropertyValue()), literal(PropertyValue(3)))), PropertyValue());
	EXPECT_EQ(evaluateFunction("range", arguments(literal(PropertyValue(1)), literal(PropertyValue(5)), literal(PropertyValue(2)))),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(3), PropertyValue(5)}));
	EXPECT_EQ(evaluateFunction("range", arguments(literal(PropertyValue(5)), literal(PropertyValue(1)), literal(PropertyValue(-2)))),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(5), PropertyValue(3), PropertyValue(1)}));
	EXPECT_THROW(static_cast<void>(evaluateFunction("range", arguments(literal(PropertyValue(1)), literal(PropertyValue(2)), literal(PropertyValue(0))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluateFunction("range", arguments(literal(PropertyValue("not-a-number")), literal(PropertyValue(2))))),
				 std::runtime_error);

	EXPECT_EQ(evaluateFunction("toBoolean", arguments(literal(PropertyValue()))), PropertyValue());
	EXPECT_EQ(evaluateFunction("toBoolean", arguments(literal(PropertyValue(true)))), PropertyValue(true));
	EXPECT_EQ(evaluateFunction("toBoolean", arguments(literal(PropertyValue(int64_t{0})))), PropertyValue(false));
	EXPECT_EQ(evaluateFunction("toBoolean", arguments(literal(PropertyValue(2.5)))), PropertyValue(true));
	EXPECT_EQ(evaluateFunction("toBoolean", arguments(literal(PropertyValue("TRUE")))), PropertyValue(true));
	EXPECT_EQ(evaluateFunction("toBoolean", arguments(literal(PropertyValue("false")))), PropertyValue(false));
	EXPECT_THROW(static_cast<void>(evaluateFunction("toBoolean", arguments(literal(PropertyValue("maybe"))))), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluateFunction("toBoolean", arguments(literal(PropertyValue(std::numeric_limits<double>::quiet_NaN()))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluateFunction("toBoolean", arguments(literal(PropertyValue(PropertyValue::MapType{{"x", PropertyValue(1)}}))))),
				 std::runtime_error);

	const auto list = [](std::vector<PropertyValue> values) {
		return PropertyValue(std::move(values));
	};
	const PropertyValue validPath = list({
			PropertyValue(PropertyValue::MapType{{"_type", PropertyValue("node")}}),
			PropertyValue(PropertyValue::MapType{{"_type", PropertyValue("relationship")}}),
			PropertyValue(PropertyValue::MapType{{"_type", PropertyValue("node")}})});
	EXPECT_EQ(evaluateFunction("length", arguments(literal(validPath))), PropertyValue(1));
	EXPECT_EQ(evaluateFunction("length", arguments(literal(list({PropertyValue(1), PropertyValue(2)})))), PropertyValue(2));
	const PropertyValue malformedPath = list({PropertyValue(PropertyValue::MapType{{"other", PropertyValue(1)}})});
	EXPECT_EQ(evaluateFunction("length", arguments(literal(malformedPath))), PropertyValue(1));

	const auto listExpression = std::make_unique<expressions::ListLiteralExpression>(list({PropertyValue(10), PropertyValue(20), PropertyValue(30)}));
	std::vector<logical::LogicalProjectItem> sliceItems;
	sliceItems.emplace_back(std::make_shared<expressions::ListSliceExpression>(
			listExpression->clone(), literal(PropertyValue(-1)), nullptr, false), "last");
	sliceItems.emplace_back(std::make_shared<expressions::ListSliceExpression>(
			listExpression->clone(), nullptr, literal(PropertyValue(2)), true), "prefix");
	logical::LogicalProject slices(std::make_unique<logical::LogicalSingleRow>(), std::move(sliceItems));
	std::vector<LogicalPlanReadRow> rows;
	static_cast<void>(runtime.execute(slices, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
		rows.push_back(row);
		return true;
	}));
	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("last")), PropertyValue(30));
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("prefix")),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(10), PropertyValue(20)}));

	const auto quantifier = [&runtime](std::string name, bool expected) {
		const auto values = std::make_unique<expressions::ListLiteralExpression>(
				PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("item"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		auto expression = std::make_shared<expressions::QuantifierFunctionExpression>(
				std::move(name), "item", values->clone(), predicate->clone());
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(expression), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		PropertyValue result;
		static_cast<void>(runtime.execute(root, runtimeBounds(), [&result](const LogicalPlanReadRow &row) {
			result = std::get<PropertyValue>(row.values.at("value"));
			return true;
		}));
		EXPECT_EQ(result, PropertyValue(expected));
	};
	quantifier("all", true);
	quantifier("any", true);
	quantifier("none", false);
	quantifier("single", false);
}

TEST(LogicalGraphReadExecutorTest, RejectsInvalidBoundsVisitorsAndPredicatesBeforeOpeningTheBackend) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.bounds.maximum_rows = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);

	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(executor.execute(request, LogicalGraphReadExecutor::RowVisitor{})),
				 std::invalid_argument);

	request.source_filter.required_labels = {""};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
	request.source_filter.required_labels = {"Person", "Person"};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
	request.source_filter.required_labels.clear();

	request.source_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_RANGE_CLOSED, PropertyValue(1))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
	request.source_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(1), PropertyValue(2))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, RejectsUnknownExpansionDirectionsAndSupportsUntypedRelationshipFilters) {
	LogicalGraphFixtureData data = graphData();
	data.edges.at(11U).type.clear();
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	LogicalGraphExpansion expansion;
	expansion.direction = static_cast<LogicalGraphReadDirection>(0);
	request.expansion = expansion;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);

	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "";
	request.expansion = std::move(expansion);
	EXPECT_EQ(executor.execute(request, [](const LogicalGraphReadRow &) { return true; }).rows_emitted, 1U);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatViolateLogicalEntityAndPropertyInvariants) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).labels = {"Person", "Person"};
	TestLogicalGraphBackend duplicateLabels(std::move(data));
	LogicalGraphReadExecutor duplicateExecutor(duplicateLabels);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(duplicateExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
	TestLogicalGraphBackend wrongPropertyOwner(std::move(data));
	LogicalGraphReadExecutor propertyExecutor(wrongPropertyOwner);
	request.source_filter.required_labels = {"VIP"};
	request.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(propertyExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).properties = {{"", PropertyValue(42)}};
	TestLogicalGraphBackend zeroPropertyId(std::move(data));
	LogicalGraphReadExecutor zeroPropertyIdExecutor(zeroPropertyId);
	EXPECT_THROW(static_cast<void>(zeroPropertyIdExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	data = graphData();
	data.edges.at(10U).source_node_id = 2U;
	data.force_invalid_adjacency = true;
	TestLogicalGraphBackend wrongDirection(std::move(data));
	LogicalGraphReadExecutor directionExecutor(wrongDirection);
	request.source_filter.property_predicates.clear();
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	EXPECT_THROW(static_cast<void>(directionExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatIgnoreTheRequestedScanBoundOrReturnNoSnapshot) {
	LogicalGraphFixtureData data = graphData();
	data.ignore_node_bound = true;
	TestLogicalGraphBackend boundViolating(std::move(data));
	LogicalGraphReadExecutor boundExecutor(boundViolating);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.bounds.maximum_node_records = 2U;
	EXPECT_THROW(static_cast<void>(boundExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	TestLogicalGraphBackend noSnapshot(graphData(), true);
	LogicalGraphReadExecutor noSnapshotExecutor(noSnapshot);
	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(noSnapshotExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatIgnoreTheRequestedAdjacencyBound) {
	LogicalGraphFixtureData data = graphData();
	data.ignore_edge_bound = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 1U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatIgnoreTraversalStopRequests) {
	LogicalGraphFixtureData data = graphData();
	data.ignore_stop_signal = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.bounds.maximum_rows = 1U;

	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);

	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsEveryBoundedFilterShapeAndPredicateOperation) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();

	request.source_filter.property_predicates = {
			predicate("", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.source_filter.property_predicates = {
			predicate("age", static_cast<LogicalPropertyPredicateOp>(255U), PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	for (const LogicalPropertyPredicateOp operation: {LogicalPropertyPredicateOp::LPP_NE, LogicalPropertyPredicateOp::LPP_LT,
													 LogicalPropertyPredicateOp::LPP_LE, LogicalPropertyPredicateOp::LPP_GT,
													 LogicalPropertyPredicateOp::LPP_GE}) {
		request.source_filter.required_labels = {"VIP"};
		request.source_filter.property_predicates = {predicate("age", operation, PropertyValue(42))};
		EXPECT_EQ(executor.execute(request, [](const LogicalGraphReadRow &) {}).rows_emitted,
				  operation == LogicalPropertyPredicateOp::LPP_NE || operation == LogicalPropertyPredicateOp::LPP_LT ||
						  operation == LogicalPropertyPredicateOp::LPP_GT
						  ? 0U
						  : 1U);
	}

	request.source_filter.required_labels.assign(1025U, "ignored");
	request.source_filter.property_predicates.clear();
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.source_filter.required_labels.clear();
	request.source_filter.property_predicates.assign(1025U, predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42)));
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.source_filter.property_predicates.clear();
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.property_predicates.assign(1025U,
																predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2)));
	request.expansion = std::move(expansion);
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.expansion->edge_filter.property_predicates.clear();
	request.bounds.maximum_node_records = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);
	request.bounds = bounds();
	request.bounds.maximum_property_entries = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedNodeScanDataAndPredicates) {
	const auto execute = [](LogicalGraphFixtureData data, logical::LogicalNodeScan scan) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(),
																[](const LogicalPlanReadRow &) { return true; })),
				 std::invalid_argument);
	};

	LogicalGraphFixtureData malformedNode = graphData();
	malformedNode.enable_exact_node_lookup = true;
	LogicalGraphNode zeroIdNode = malformedNode.nodes.at(1U);
	zeroIdNode.node_id = 0U;
	malformedNode.exact_lookup_result = std::vector<LogicalGraphNode>{zeroIdNode};
	TestLogicalGraphBackend malformedBackend(std::move(malformedNode));
	LogicalPlanReadRuntime malformedRuntime(malformedBackend);
	logical::LogicalNodeScan validScan("n", {"Person"}, {{"age", PropertyValue(42)}});
	EXPECT_THROW(static_cast<void>(malformedRuntime.execute(validScan, runtimeBounds(),
																					 [](const LogicalPlanReadRow &) { return true; })),
				 std::runtime_error);
	const auto expectMalformedIndexedNode = [&validScan](LogicalGraphNode node) {
		LogicalGraphFixtureData data = graphData();
		data.enable_exact_node_lookup = true;
		data.exact_lookup_result = std::vector<LogicalGraphNode>{std::move(node)};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		EXPECT_THROW(static_cast<void>(runtime.execute(validScan, runtimeBounds(),
																					 [](const LogicalPlanReadRow &) { return true; })),
				 std::runtime_error);
	};

	LogicalGraphNode emptyLabelNode = graphData().nodes.at(1U);
	emptyLabelNode.labels = {""};
	expectMalformedIndexedNode(std::move(emptyLabelNode));
	LogicalGraphNode duplicateLabelNode = graphData().nodes.at(1U);
	duplicateLabelNode.labels = {"Person", "Person"};
	expectMalformedIndexedNode(std::move(duplicateLabelNode));
	LogicalGraphNode emptyPropertyKeyNode = graphData().nodes.at(1U);
	emptyPropertyKeyNode.properties = {{"", PropertyValue(42)}};
	expectMalformedIndexedNode(std::move(emptyPropertyKeyNode));
	LogicalGraphNode duplicatePropertyKeyNode = graphData().nodes.at(1U);
	duplicatePropertyKeyNode.properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
	expectMalformedIndexedNode(std::move(duplicatePropertyKeyNode));

	execute(graphData(), logical::LogicalNodeScan("n", {""}));
	execute(graphData(), logical::LogicalNodeScan("n", {"VIP", "VIP"}));
	execute(graphData(), logical::LogicalNodeScan("n", {}, {{"", PropertyValue(42)}}));

	logical::LogicalNodeScan emptyRangeKey("n");
	emptyRangeKey.setRangePredicates({{"", PropertyValue(), PropertyValue(), true, true}});
	execute(graphData(), std::move(emptyRangeKey));

	logical::LogicalNodeScan malformedComposite("n");
	malformedComposite.setCompositeEquality({{}, {}});
	execute(graphData(), std::move(malformedComposite));

	logical::LogicalNodeScan mismatchedComposite("n");
	mismatchedComposite.setCompositeEquality({{"age"}, {}});
	execute(graphData(), std::move(mismatchedComposite));

	logical::LogicalNodeScan emptyCompositeKey("n");
	emptyCompositeKey.setCompositeEquality({{""}, {PropertyValue(42)}});
	execute(graphData(), std::move(emptyCompositeKey));
}

TEST(LogicalGraphReadExecutorTest, RejectsMalformedPropertyCollectionsAndLogicalEntityIdentities) {
	const auto expectMalformedSourceProperties = [](LogicalGraphFixtureData data) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalGraphReadExecutor executor(backend);
		LogicalGraphReadRequest request;
		request.bounds = bounds();
		request.source_filter.required_labels = {"VIP"};
		request.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
		EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);
	};

	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.push_back({"extra", PropertyValue(1)});
	LogicalGraphReadExecutionBounds propertyBound = bounds();
	propertyBound.maximum_property_entries = 2U;
	TestLogicalGraphBackend oversizedProperties(std::move(data));
	LogicalGraphReadExecutor oversizedPropertyExecutor(oversizedProperties);
	LogicalGraphReadRequest propertyRequest;
	propertyRequest.bounds = propertyBound;
	propertyRequest.source_filter.required_labels = {"VIP"};
	propertyRequest.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(oversizedPropertyExecutor.execute(propertyRequest, [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).properties = {{"", PropertyValue(42)}};
	expectMalformedSourceProperties(std::move(data));

	data = graphData();
	data.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
	expectMalformedSourceProperties(std::move(data));

	data = graphData();
	data.nodes.at(1U).labels = {"", "VIP"};
	TestLogicalGraphBackend emptyNodeLabel(std::move(data));
	LogicalGraphReadExecutor emptyNodeLabelExecutor(emptyNodeLabel);
	LogicalGraphReadRequest emptyLabelRequest;
	emptyLabelRequest.bounds = bounds();
	EXPECT_THROW(static_cast<void>(emptyNodeLabelExecutor.execute(emptyLabelRequest, [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).node_id = 0U;
	TestLogicalGraphBackend zeroNode(std::move(data));
	LogicalGraphReadExecutor zeroNodeExecutor(zeroNode);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(zeroNodeExecutor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);

	const auto edgeRequest = [] {
		LogicalGraphReadRequest result;
		result.bounds = bounds();
		result.source_filter.required_labels = {"VIP"};
		LogicalGraphExpansion expansion;
		expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
		result.expansion = std::move(expansion);
		return result;
	};

	data = graphData();
	data.edges.at(10U).edge_id = 0U;
	TestLogicalGraphBackend zeroEdge(std::move(data));
	LogicalGraphReadExecutor zeroEdgeExecutor(zeroEdge);
	EXPECT_THROW(static_cast<void>(zeroEdgeExecutor.execute(edgeRequest(), [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.edges.at(10U).source_node_id = 0U;
	data.force_invalid_adjacency = true;
	TestLogicalGraphBackend zeroEdgeSource(std::move(data));
	LogicalGraphReadExecutor zeroEdgeSourceExecutor(zeroEdgeSource);
	EXPECT_THROW(static_cast<void>(zeroEdgeSourceExecutor.execute(edgeRequest(), [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.edges.at(10U).target_node_id = 0U;
	TestLogicalGraphBackend zeroEdgeTarget(std::move(data));
	LogicalGraphReadExecutor zeroEdgeTargetExecutor(zeroEdgeTarget);
	EXPECT_THROW(static_cast<void>(zeroEdgeTargetExecutor.execute(edgeRequest(), [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, OmitsEdgesWithoutPropertiesAndTargetsThatMissTheExpansionFilter) {
	LogicalGraphFixtureData data = graphData();
	data.edges.at(10U).properties.clear();
	TestLogicalGraphBackend propertylessBackend(std::move(data));
	LogicalGraphReadExecutor propertylessExecutor(propertylessBackend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.property_predicates = {predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	request.expansion = std::move(expansion);
	EXPECT_EQ(propertylessExecutor.execute(request, [](const LogicalGraphReadRow &) {}).rows_emitted, 0U);

	TestLogicalGraphBackend targetBackend(graphData());
	LogicalGraphReadExecutor targetExecutor(targetBackend);
	request.expansion->edge_filter.property_predicates.clear();
	request.expansion->target_filter.required_labels = {"VIP"};
	EXPECT_EQ(targetExecutor.execute(request, [](const LogicalGraphReadRow &) {}).rows_emitted, 0U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeJoinsCompatibleRowsAndSkipsEmptyOrConflictingInputs) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto literal = [](int64_t value) {
		return sharedExpression(std::make_unique<expressions::LiteralExpression>(value));
	};
	const auto project = [&literal](int64_t shared, std::string alias, int64_t unique) {
		return std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSingleRow>(),
				std::vector<logical::LogicalProjectItem>{{literal(shared), "shared"}, {literal(unique), std::move(alias)}});
	};

	logical::LogicalJoin compatible(project(1, "left", 2), project(1, "right", 3));
	std::vector<LogicalPlanReadRow> rows;
	const LogicalPlanReadRuntimeStats compatibleStats =
			runtime.execute(compatible, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
				rows.push_back(row);
				return true;
			});
	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("shared")), PropertyValue(1));
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("left")), PropertyValue(2));
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("right")), PropertyValue(3));
	EXPECT_EQ(compatibleStats.rows_emitted, 1U);
	EXPECT_EQ(runtime.execute(compatible, runtimeBounds(), [](const LogicalPlanReadRow &) { return false; }).rows_emitted,
			  1U);

	logical::LogicalJoin conflicting(project(1, "left", 2), project(9, "right", 3));
	EXPECT_EQ(runtime.execute(conflicting, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; }).rows_emitted,
			  0U);

	auto emptyRight = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalSingleRow>(),
			sharedExpression(std::make_unique<expressions::LiteralExpression>(false)));
	logical::LogicalJoin empty(project(1, "left", 2), std::move(emptyRight));
	EXPECT_EQ(runtime.execute(empty, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; }).rows_emitted,
			  0U);
}

TEST(LogicalGraphReadExecutorTest, RejectsTopologyTypesBeforeReadingTargetLabels) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.enable_native_adjacent_topology_pages = true;
	const auto labelReads = std::make_shared<size_t>(0U);
	const auto fullScans = std::make_shared<size_t>(0U);
	data.node_label_batch_read_count = labelReads;
	data.edge_record_scan_count = fullScans;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "UNMATCHED";
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = expansion;
	request.bounds = bounds();
	size_t rows = 0U;
	const auto stats = executor.execute(request, [&](const LogicalGraphReadRow &) { ++rows; return true; });
	EXPECT_EQ(rows, 0U);
	EXPECT_EQ(stats.edges_scanned, 2U);
	EXPECT_EQ(*labelReads, 0U);
	EXPECT_EQ(*fullScans, 0U);
}

TEST(LogicalGraphReadExecutorTest, RejectsRelationshipTypesBeforeSelectedEdgePropertyColumns) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	const auto columns = std::make_shared<size_t>(0U);
	data.edge_property_column_read_count = columns;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "UNMATCHED";
	expansion.edge_filter.property_predicates = {predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	request.expansion = expansion;
	request.bounds = bounds();
	for (int retry = 0; retry < 2; ++retry) {
		size_t rows = 0U;
		const auto stats = executor.execute(request, [&](const LogicalGraphReadRow &) { ++rows; return true; });
		EXPECT_EQ(rows, 0U);
		EXPECT_EQ(stats.edges_scanned, 2U);
		EXPECT_EQ(*columns, 0U);
	}
}
