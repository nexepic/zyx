/**
 * @file CypherASTBuilder.hpp
 * @brief Converts ANTLR parse tree contexts into QueryAST structures.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/ir/QueryAST.hpp"

namespace graph::query::ir {

/**
 * @class CypherASTBuilder
 * @brief Builds QueryAST structures from ANTLR4 parse tree contexts.
 *
 * Uses ExpressionBuilder for expression construction.
 *
 * The void* parameters accept ANTLR parse tree contexts to avoid
 * pulling ANTLR headers into the public interface.
 */
class CypherASTBuilder {
public:
	// --- Projection Clauses ---
	static ReturnClause buildReturnClause(void* ctx);
	static WithClause buildWithClause(void* ctx);

	// --- Reading Clauses ---
	static MatchClause buildMatchClause(void* ctx);
	static UnwindClause buildUnwindClause(void* ctx);
	static CallClause buildStandaloneCallClause(void* ctx);
	static CallClause buildInQueryCallClause(void* ctx);

	// --- Writing Clauses ---
	static CreateClause buildCreateClause(void* ctx);
	static SetClause buildSetClause(void* ctx);
	static DeleteClause buildDeleteClause(void* ctx);
	static RemoveClause buildRemoveClause(void* ctx);
	static MergeClause buildMergeClause(void* ctx);

	// --- Admin Clauses ---
	static ShowIndexesClause buildShowIndexesClause();
	static CreateIndexClause buildCreateIndexByPatternClause(void* ctx);
	static CreateIndexClause buildCreateIndexByLabelClause(void* ctx);
	static CreateVectorIndexClause buildCreateVectorIndexClause(void* ctx);
	static DropIndexClause buildDropIndexByNameClause(void* ctx);
	static DropIndexClause buildDropIndexByLabelClause(void* ctx);
	static CreateConstraintClause buildCreateNodeConstraintClause(void* ctx);
	static CreateConstraintClause buildCreateEdgeConstraintClause(void* ctx);
	static DropConstraintClause buildDropConstraintClause(const std::string& name, bool ifExists);
	static ShowConstraintsClause buildShowConstraintsClause();

	// --- Pattern helpers (used internally) ---
	static PatternElementAST extractPatternElement(void* ctx);
	static std::vector<SetItemAST> extractSetItems(void* ctx);
};

} // namespace graph::query::ir
