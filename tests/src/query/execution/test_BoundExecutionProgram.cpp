#include <gtest/gtest.h>
#include "CypherParserImpl.hpp"
#include "query/execution/BoundExecutionProgram.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"

namespace graph::query::execution::detail {
namespace {
class ProgramSnapshot final : public storage::LogicalGraphReadSnapshot {
public:
	std::optional<storage::LogicalGraphSchemaIdentity> identity{storage::LogicalGraphSchemaIdentity{1U, 2U, {}, 0U, 0U}};
	bool tokens = true;
	std::optional<storage::LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override { return identity; }
	bool supportsPropertyTokenResolution() const override { return tokens; }
	storage::LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("not used"); }
	storage::LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("not used"); }
	storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
	storage::LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
		std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override { return {}; }
	void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
		const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
	void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {}
	void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
};
}

TEST(BoundExecutionProgramTest, SeparatesOperatorLayoutsAndRejectsForeignDescriptorsAndClones) {
	parser::cypher::CypherParserImpl parser;
	auto plan = parser.parseToLogical("MATCH (n) WITH n AS x RETURN x.value AS value");
	ProgramSnapshot snapshot; MaterializedByteBudget budget(65536U);
	{
		BoundExecutionProgram program(*plan.root, snapshot, budget, [] {});
		ASSERT_EQ(program.size(), 3U);
		const auto root = program.view(program.root()); ASSERT_EQ(root.inputs.size(), 1U); ASSERT_EQ(root.outputs.size(), 1U);
		EXPECT_EQ(program.name(root.inputs[0]), "x"); EXPECT_EQ(program.name(root.outputs[0]), "value");
		const auto child = program.view(root.children[0]); ASSERT_EQ(child.inputs.size(), 1U); ASSERT_EQ(child.outputs.size(), 1U);
		EXPECT_EQ(program.name(child.inputs[0]), "n"); EXPECT_EQ(program.name(child.outputs[0]), "x");
		EXPECT_NE(root.inputs[0], child.outputs[0]);
		EXPECT_GT(budget.used(), 0U);
		auto clone = plan.clone(); EXPECT_THROW(static_cast<void>(program.id(*clone.root)), std::invalid_argument);
		BoundExecutionProgram other(*plan.root, snapshot, budget, [] {});
		EXPECT_THROW(static_cast<void>(program.view(other.root())), std::invalid_argument);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundExecutionProgramTest, CorrelatedImportsRemainExplicitAndDoNotLeakUnimportedNames) {
	auto input = std::make_unique<logical::LogicalNodeScan>("n");
	std::vector<logical::LogicalProjectItem> items{{std::make_shared<expressions::VariableReferenceExpression>("n"), "m"}};
	auto inner = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	logical::LogicalCallSubquery root(std::move(input), std::move(inner), {"n"}, {"m"});
	ProgramSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(root, snapshot, budget, [] {});
	const auto call = program.view(program.root()); ASSERT_EQ(call.children.size(), 2U);
	const auto subquery = program.view(call.children[1]);
	EXPECT_TRUE(program.input(call.children[1], "n")); EXPECT_FALSE(program.input(call.children[1], "private"));
	const auto leaf = program.view(subquery.children[0]); ASSERT_EQ(leaf.inputs.size(), 1U);
	EXPECT_EQ(program.name(leaf.inputs[0]), "n");
	EXPECT_NE(leaf.inputs[0], call.inputs[0]);
}

TEST(BoundExecutionProgramTest, SchemaDatabaseOverlayAndCapabilityChangesRequireRebinding) {
	logical::LogicalNodeScan root("n"); ProgramSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(root, snapshot, budget, [] {});
	EXPECT_NO_THROW(program.validateSnapshot(snapshot));
	const auto original = snapshot.identity;
	for (int variant = 0; variant < 5; ++variant) {
		snapshot.identity = original;
		if (variant == 0) ++snapshot.identity->database_token;
		if (variant == 1) ++snapshot.identity->format_token;
		if (variant == 2) ++snapshot.identity->catalog_tokens[0];
		if (variant == 3) snapshot.identity->view_token = 1U;
		if (variant == 4) snapshot.identity.reset();
		EXPECT_THROW(program.validateSnapshot(snapshot), std::runtime_error);
	}
	snapshot.identity = original; snapshot.tokens = false;
	EXPECT_THROW(program.validateSnapshot(snapshot), std::runtime_error);
}

TEST(BoundExecutionProgramTest, AnonymousSchemaReadersAreSupportedWithoutInventingTokens) {
	logical::LogicalNodeScan root("n"); ProgramSnapshot snapshot; snapshot.identity.reset(); snapshot.tokens = false;
	MaterializedByteBudget budget(65536U); BoundExecutionProgram program(root, snapshot, budget, [] {});
	EXPECT_NO_THROW(program.validateSnapshot(snapshot)); EXPECT_TRUE(program.input(program.root(), "n"));
}

TEST(BoundExecutionProgramTest, AdmissionCancellationAndWorkLimitsRefundAllOwnedMemory) {
	parser::cypher::CypherParserImpl parser;
	auto plan = parser.parseToLogical("MATCH (n) RETURN n.value AS value"); ProgramSnapshot snapshot;
	MaterializedByteBudget tiny(1U);
	EXPECT_THROW((BoundExecutionProgram(*plan.root, snapshot, tiny, [] {})), MaterializedByteLimitExceeded);
	EXPECT_EQ(tiny.used(), 0U);
	MaterializedByteBudget budget(65536U);
	size_t calls = 0U;
	{ BoundExecutionProgram program(*plan.root, snapshot, budget, [&] { ++calls; }); }
	for (size_t stop = 1U; stop <= calls; ++stop) {
		size_t seen = 0U;
		EXPECT_THROW((BoundExecutionProgram(*plan.root, snapshot, budget, [&] { if (++seen == stop) throw std::runtime_error("cancelled"); })), std::runtime_error);
		EXPECT_EQ(budget.used(), 0U);
	}
	EXPECT_THROW((BoundExecutionProgram(*plan.root, snapshot, budget, [] {}, {.operators = 1U})), std::runtime_error);
	EXPECT_EQ(budget.used(), 0U);
}
TEST(BoundExecutionProgramTest, InjectedInputsAreExplicitSealedSlotsAcrossStandaloneAndNestedOperators) {
	std::vector<logical::LogicalProjectItem> innerItems{{std::make_shared<expressions::VariableReferenceExpression>("external"), "x"}};
	auto inner = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(innerItems));
	std::vector<logical::LogicalProjectItem> outerItems{{std::make_shared<expressions::VariableReferenceExpression>("x"), "value"}};
	logical::LogicalProject root(std::move(inner), std::move(outerItems));
	ProgramSnapshot snapshot; MaterializedByteBudget budget(65536U);
	const std::array<std::string_view, 1U> injected{"external"};
	BoundExecutionProgram program(root, snapshot, budget, [] {}, {}, std::pmr::get_default_resource(), injected);
	const auto outer = program.view(program.root()); ASSERT_TRUE(program.input(program.root(), "x"));
	const auto project = program.view(outer.children[0]); ASSERT_TRUE(program.input(outer.children[0], "external"));
	EXPECT_NE(*program.input(program.root(), "x"), *program.input(outer.children[0], "external"));
	ASSERT_EQ(project.children.size(), 1U); EXPECT_TRUE(program.input(project.children[0], "external"));
}

}
