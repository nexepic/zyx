/**
 * @file QueryAST.hpp
 * @brief Clause-level IR for Cypher queries.
 *
 * Captures the structure of all Cypher clauses in a form suitable for
 * semantic analysis, without coupling to ANTLR parse trees or logical operators.
 * Reuses the existing Expression hierarchy for expression nodes.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "graph/core/PropertyTypes.hpp"

namespace graph::query::expressions {
class Expression;
} // namespace graph::query::expressions

namespace graph::query::ir {

/**
 * @struct ProjectionItem
 * @brief A single item in a RETURN or WITH projection list.
 *
 * Semantic annotations (containsAggregate, isTopLevelAggregate) are
 * filled by SemanticAnalyzer::analyze(), not at construction time.
 */
struct ProjectionItem {
	std::shared_ptr<expressions::Expression> expression;
	std::string alias;

	// Filled by SemanticAnalyzer:
	bool containsAggregate = false;
	bool isTopLevelAggregate = false;  // count(n) vs count(n)+1
};

/**
 * @struct SortItem
 * @brief A single ORDER BY item.
 */
struct SortItem {
	std::shared_ptr<expressions::Expression> expression;
	bool ascending = true;
};

/**
 * @struct ProjectionBody
 * @brief Shared structure for RETURN and WITH projection bodies.
 *
 * Contains projection items, ORDER BY, SKIP, LIMIT, DISTINCT.
 * SemanticAnalyzer annotates hasAggregates after analysis.
 */
struct ProjectionBody {
	std::vector<ProjectionItem> items;
	std::vector<SortItem> orderBy;
	std::optional<int64_t> skip;
	std::optional<int64_t> limit;
	bool distinct = false;
	bool returnStar = false;

	// Filled by SemanticAnalyzer:
	bool hasAggregates = false;
};

/**
 * @struct ReturnClause
 * @brief Represents a RETURN clause.
 */
struct ReturnClause {
	ProjectionBody body;
};

/**
 * @struct WithClause
 * @brief Represents a WITH clause, optionally followed by WHERE.
 */
struct WithClause {
	ProjectionBody body;
	std::shared_ptr<expressions::Expression> where;
};

// =============================================================================
// Pattern AST types
// =============================================================================

struct NodePatternAST {
	std::string variable;
	std::vector<std::string> labels;
	std::vector<std::pair<std::string, graph::PropertyValue>> properties;
	// Expression-based properties (for CREATE with non-literal values)
	std::unordered_map<std::string, std::shared_ptr<expressions::Expression>> propertyExpressions;
};

struct RelationshipPatternAST {
	std::string variable;
	std::string type;
	std::string direction; // "in", "out", "both"
	std::unordered_map<std::string, graph::PropertyValue> properties;
	// Expression-based properties (for MATCH/CREATE with non-literal values)
	std::unordered_map<std::string, std::shared_ptr<expressions::Expression>> propertyExpressions;
	bool isVarLength = false;
	int minHops = 1;
	int maxHops = 1;
};

struct PatternChainAST {
	RelationshipPatternAST relationship;
	NodePatternAST targetNode;
};

struct PatternElementAST {
	NodePatternAST headNode;
	std::vector<PatternChainAST> chain;
};

struct PatternPartAST {
	std::string pathVariable; // for named paths: p = (a)-[r]->(b)
	PatternElementAST element;
};

// =============================================================================
// Set/Remove item AST types
// =============================================================================

enum class SetItemType { SIT_PROPERTY, SIT_LABEL, SIT_MAP_MERGE };

struct SetItemAST {
	SetItemType type;
	std::string variable;
	std::string key;
	std::shared_ptr<expressions::Expression> expression;
};

enum class RemoveItemType { RIT_PROPERTY, RIT_LABEL };

struct RemoveItemAST {
	RemoveItemType type;
	std::string variable;
	std::string key;
};

// =============================================================================
// DML clause AST types
// =============================================================================

struct MatchClause {
	std::vector<PatternPartAST> patterns;
	std::shared_ptr<expressions::Expression> where;
	bool optional = false;
};

struct CreateClause {
	std::vector<PatternPartAST> patterns;
};

struct SetClause {
	std::vector<SetItemAST> items;
};

struct DeleteClause {
	std::vector<std::string> variables;
	bool detach = false;
};

struct RemoveClause {
	std::vector<RemoveItemAST> items;
};

struct MergeClause {
	PatternPartAST pattern;
	std::vector<SetItemAST> onCreateItems;
	std::vector<SetItemAST> onMatchItems;
};

struct UnwindClause {
	std::string alias;
	std::shared_ptr<expressions::Expression> expression;
	std::vector<graph::PropertyValue> literalList; // non-empty if literal list
};

struct CallClause {
	std::string procedureName;
	std::vector<graph::PropertyValue> arguments;
	bool standalone = false;
	struct YieldItem {
		std::string sourceField;
		std::string alias;
	};
	std::vector<YieldItem> yieldItems;
};

// =============================================================================
// Admin clause AST types
// =============================================================================

struct ShowIndexesClause {};

struct CreateIndexClause {
	std::string name;
	std::string label;
	std::vector<std::string> properties;
};

struct DropIndexClause {
	std::string name;
	std::string label;
	std::string property;
};

struct CreateVectorIndexClause {
	std::string name;
	std::string label;
	std::string property;
	int dimension = 0;
	std::string metric = "L2";
};

struct CreateConstraintClause {
	std::string name;
	std::string targetType; // "node" or "edge"
	std::string constraintType; // "unique", "not_null", "property_type", "node_key"
	std::string label;
	std::vector<std::string> properties;
	std::string typeName; // for property_type constraints
};

struct DropConstraintClause {
	std::string name;
	bool ifExists = false;
};

struct ShowConstraintsClause {};

} // namespace graph::query::ir
