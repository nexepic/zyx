#include <gtest/gtest.h>
#include "CypherParserImpl.hpp"
#include "query/execution/BoundExpressionInputs.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"

namespace graph::query::execution::detail {
namespace {
class ExpressionBindingSnapshot final : public storage::LogicalGraphReadSnapshot {
public:
	storage::LogicalGraphSchemaIdentity identity{1U, 2U, {}, 0U, 0U};
	mutable size_t resolutions = 0U;
	std::optional<storage::LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override { return identity; }
	bool supportsPropertyTokenResolution() const override { return true; }
	std::optional<uint32_t> findPropertyToken(std::string_view key) const override { ++resolutions; return key == "rank" ? std::optional<uint32_t>(7U) : std::nullopt; }
	storage::LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected data read"); }
	storage::LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected data read"); }
	void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {}
	void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
	storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
	storage::LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
		std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override { return {}; }
	void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
		const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
};
}

TEST(BoundExpressionInputsTest, NestedReferencesUseSlotsAndShareAuthenticatedPropertyDescriptors) {
	parser::cypher::CypherParserImpl parser; auto plan = parser.parseToLogical("MATCH (n) RETURN n.rank + n.rank AS total,n.rank AS rank");
	const auto &project = dynamic_cast<const logical::LogicalProject &>(*plan.root);
	const auto &binary = dynamic_cast<const expressions::BinaryOpExpression &>(*project.getItems()[0].expression);
	const std::array<const expressions::Expression *, 2U> expressions{project.getItems()[0].expression.get(), project.getItems()[1].expression.get()};
	ExpressionBindingSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(*plan.root, snapshot, budget, [] {});
	BoundExpressionInputs inputs(program, program.root(), expressions, snapshot, budget, [] {});
	const auto *left = inputs.instruction(*binary.getLeft()), *right = inputs.instruction(*binary.getRight());
	ASSERT_NE(left, nullptr); ASSERT_NE(right, nullptr); EXPECT_EQ(left->source, right->source); EXPECT_EQ(left->property, right->property);
	EXPECT_EQ(snapshot.resolutions, 1U);
	LogicalPlanReadRow row; row.values.emplace("n", storage::EntityRef{1U, false, 0U, 0U, 0U});
	{ BoundExpressionInputs::RowBinding binding(inputs, row);
		EXPECT_EQ(std::get<storage::EntityRef>(*inputs.value(*left)).entity_id, 1U);
		const auto property = inputs.property(*left, snapshot); ASSERT_TRUE(property); EXPECT_EQ(property->key, "rank"); EXPECT_EQ(property->token, 7U);
		EXPECT_THROW((BoundExpressionInputs::RowBinding(inputs, row)), std::logic_error);
	}
	EXPECT_THROW(static_cast<void>(inputs.value(*left)), std::logic_error);
	row.values.at("n") = storage::EntityRef{2U, false, 0U, 0U, 0U};
	{ BoundExpressionInputs::RowBinding binding(inputs, row); EXPECT_EQ(std::get<storage::EntityRef>(*inputs.value(*left)).entity_id, 2U); }
	EXPECT_EQ(snapshot.resolutions, 1U);
}

TEST(BoundExpressionInputsTest, ProjectedPropertyTextWinsAndLocalShadowingRemainsADeclaredScopeAdapter) {
	parser::cypher::CypherParserImpl parser;
	auto plan = parser.parseToLogical("MATCH (n) WITH n.rank RETURN n.rank AS rank");
	const auto &project = dynamic_cast<const logical::LogicalProject &>(*plan.root);
	const expressions::Expression *expression = project.getItems()[0].expression.get();
	ExpressionBindingSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(*plan.root, snapshot, budget, [] {});
	BoundExpressionInputs inputs(program, program.root(), std::span(&expression, 1U), snapshot, budget, [] {});
	const auto *instruction = inputs.instruction(*expression); ASSERT_NE(instruction, nullptr); EXPECT_EQ(instruction->kind, BoundInputKind::BIK_VALUE);
	LogicalPlanReadRow row; row.values.emplace("n.rank", PropertyValue(19));
	{ BoundExpressionInputs::RowBinding binding(inputs, row); EXPECT_EQ(std::get<PropertyValue>(*inputs.value(*instruction)), PropertyValue(19)); }
	EXPECT_EQ(snapshot.resolutions, 0U);
	auto scoped = parser.parseToLogical("MATCH (n) WITH 9 AS x RETURN [x IN [1,2] | x + 1] AS values");
	BoundExecutionProgram scopedProgram(*scoped.root, snapshot, budget, [] {});
	const auto &scopedProject = dynamic_cast<const logical::LogicalProject &>(*scoped.root);
	expression = scopedProject.getItems()[0].expression.get();
	BoundExpressionInputs scopedInputs(scopedProgram, scopedProgram.root(), std::span(&expression, 1U), snapshot, budget, [] {});
	EXPECT_EQ(scopedInputs.instruction(*expression), nullptr);
}

TEST(BoundExpressionInputsTest, UnknownNamesRemainDeferredAndReaderChangesRejectPropertyUse) {
	parser::cypher::CypherParserImpl parser; auto plan = parser.parseToLogical("MATCH (n) RETURN CASE WHEN false THEN unknown.rank ELSE n.future END AS value");
	const auto &project = dynamic_cast<const logical::LogicalProject &>(*plan.root);
	const auto &expression = dynamic_cast<const expressions::CaseExpression &>(*project.getItems()[0].expression);
	const expressions::Expression *root = &expression;
	ExpressionBindingSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(*plan.root, snapshot, budget, [] {});
	BoundExpressionInputs inputs(program, program.root(), std::span(&root, 1U), snapshot, budget, [] {});
	const auto *unknown = inputs.instruction(*expression.getBranches()[0].thenExpression); ASSERT_NE(unknown, nullptr); EXPECT_FALSE(unknown->source);
	const auto *future = inputs.instruction(*expression.getElseExpression()); ASSERT_NE(future, nullptr);
	const auto property = inputs.property(*future, snapshot); ASSERT_TRUE(property); EXPECT_FALSE(property->token);
	++snapshot.identity.catalog_tokens[0]; EXPECT_THROW(static_cast<void>(inputs.property(*future, snapshot)), std::runtime_error);
}

TEST(BoundExpressionInputsTest, ConstructionCancellationRefundsEverythingExceptTheExistingProgram) {
	parser::cypher::CypherParserImpl parser; auto plan = parser.parseToLogical("MATCH (n) RETURN n.rank + n.rank AS rank");
	const auto &project = dynamic_cast<const logical::LogicalProject &>(*plan.root);
	const expressions::Expression *expression = project.getItems()[0].expression.get();
	ExpressionBindingSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(*plan.root, snapshot, budget, [] {}); const auto baseline = budget.used(); size_t calls = 0U;
	{ BoundExpressionInputs inputs(program, program.root(), std::span(&expression, 1U), snapshot, budget, [&] { ++calls; }); }
	EXPECT_EQ(budget.used(), baseline);
	for (size_t stop = 1U; stop <= calls; ++stop) {
		size_t seen = 0U;
		EXPECT_THROW((BoundExpressionInputs(program, program.root(), std::span(&expression, 1U), snapshot, budget,
			[&] { if (++seen == stop) throw std::runtime_error("cancelled"); })), std::runtime_error);
		EXPECT_EQ(budget.used(), baseline);
	}
}

TEST(BoundExpressionInputsTest, ParameterDescriptorsRebindValuesAndRejectAnotherContextOrInputOwner) {
	parser::cypher::CypherParserImpl parser; auto plan = parser.parseToLogical("MATCH (n) RETURN $value + $value AS total");
	const auto &project = dynamic_cast<const logical::LogicalProject &>(*plan.root);
	const auto &binary = dynamic_cast<const expressions::BinaryOpExpression &>(*project.getItems()[0].expression);
	const expressions::Expression *expression = &binary;
	ExpressionBindingSnapshot snapshot; MaterializedByteBudget budget(65536U); BoundExecutionProgram program(*plan.root, snapshot, budget, [] {});
	QueryContext first, second; first.parameters.emplace("value", PropertyValue(3)); second.parameters.emplace("value", PropertyValue("changed"));
	BoundExpressionInputs a(program, program.root(), std::span(&expression, 1U), snapshot, budget, [] {}, std::pmr::get_default_resource(), &first);
	BoundExpressionInputs b(program, program.root(), std::span(&expression, 1U), snapshot, budget, [] {}, std::pmr::get_default_resource(), &second);
	const auto *left = a.instruction(*binary.getLeft()), *right = a.instruction(*binary.getRight()); ASSERT_NE(left, nullptr); ASSERT_NE(right, nullptr);
	EXPECT_EQ(left->kind, BoundInputKind::BIK_PARAMETER); EXPECT_EQ(left->input_ordinal, right->input_ordinal);
	EXPECT_EQ(*a.parameter(*left, &first), PropertyValue(3));
	EXPECT_EQ(*b.parameter(*b.instruction(*binary.getLeft()), &second), PropertyValue("changed"));
	EXPECT_THROW(static_cast<void>(a.parameter(*left, &second)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(b.parameter(*left, &second)), std::invalid_argument);
}
}
