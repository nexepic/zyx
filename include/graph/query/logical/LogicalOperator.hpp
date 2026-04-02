/**
 * @file LogicalOperator.hpp
 * @brief Abstract base class and type enum for all logical plan operators.
 *
 * The logical plan layer sits between the Cypher AST and physical execution.
 * Logical operators are inspectable, cloneable, and rewritable by optimizer rules.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @enum LogicalOpType
 * @brief Enumerates all logical operator types.
 *
 * Prefixed with LOP_ to avoid Windows macro conflicts.
 */
enum class LogicalOpType {
	// Read operators (optimizable)
	LOP_NODE_SCAN,
	LOP_FILTER,
	LOP_PROJECT,
	LOP_AGGREGATE,
	LOP_SORT,
	LOP_LIMIT,
	LOP_SKIP,
	LOP_JOIN,
	LOP_OPTIONAL_MATCH,
	LOP_TRAVERSAL,
	LOP_VAR_LENGTH_TRAVERSAL,
	LOP_UNWIND,
	LOP_UNION,
	LOP_SINGLE_ROW,

	// Write operators (pass-through, not optimized)
	LOP_CREATE_NODE,
	LOP_CREATE_EDGE,
	LOP_SET,
	LOP_DELETE,
	LOP_REMOVE,
	LOP_MERGE_NODE,
	LOP_MERGE_EDGE,

	// DDL / Admin (pass-through)
	LOP_CREATE_INDEX,
	LOP_DROP_INDEX,
	LOP_SHOW_INDEXES,
	LOP_CREATE_VECTOR_INDEX,
	LOP_CREATE_CONSTRAINT,
	LOP_DROP_CONSTRAINT,
	LOP_SHOW_CONSTRAINTS,
	LOP_TRANSACTION_CONTROL,
	LOP_LIST_CONFIG,
	LOP_SET_CONFIG,
	LOP_CALL_PROCEDURE,
	LOP_ALGO_SHORTEST_PATH,
	LOP_VECTOR_SEARCH,
	LOP_TRAIN_VECTOR_INDEX,

	// Observability (pass-through)
	LOP_EXPLAIN,
	LOP_PROFILE
};

/**
 * @class LogicalOperator
 * @brief Abstract base class for all logical plan operators.
 *
 * Logical operators form a tree that can be inspected, cloned, and rewritten
 * by optimizer rules before being converted to physical operators.
 */
class LogicalOperator {
public:
	virtual ~LogicalOperator() = default;

	/** @brief Returns the type of this logical operator. */
	[[nodiscard]] virtual LogicalOpType getType() const = 0;

	/** @brief Returns raw pointers to child operators. */
	[[nodiscard]] virtual std::vector<LogicalOperator *> getChildren() const = 0;

	/** @brief Returns the variables produced by this operator. */
	[[nodiscard]] virtual std::vector<std::string> getOutputVariables() const = 0;

	/** @brief Creates a deep copy of this operator and its subtree. */
	[[nodiscard]] virtual std::unique_ptr<LogicalOperator> clone() const = 0;

	/** @brief Returns a human-readable description for EXPLAIN output. */
	[[nodiscard]] virtual std::string toString() const = 0;

	/** @brief Replaces a child at the given index. */
	virtual void setChild(size_t index, std::unique_ptr<LogicalOperator> child) = 0;

	/** @brief Detaches and returns ownership of a child at the given index. */
	virtual std::unique_ptr<LogicalOperator> detachChild(size_t index) = 0;
};

/**
 * @brief Returns a string name for a LogicalOpType.
 */
inline std::string toString(LogicalOpType type) {
	switch (type) {
		case LogicalOpType::LOP_NODE_SCAN: return "NodeScan";
		case LogicalOpType::LOP_FILTER: return "Filter";
		case LogicalOpType::LOP_PROJECT: return "Project";
		case LogicalOpType::LOP_AGGREGATE: return "Aggregate";
		case LogicalOpType::LOP_SORT: return "Sort";
		case LogicalOpType::LOP_LIMIT: return "Limit";
		case LogicalOpType::LOP_SKIP: return "Skip";
		case LogicalOpType::LOP_JOIN: return "Join";
		case LogicalOpType::LOP_OPTIONAL_MATCH: return "OptionalMatch";
		case LogicalOpType::LOP_TRAVERSAL: return "Traversal";
		case LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL: return "VarLengthTraversal";
		case LogicalOpType::LOP_UNWIND: return "Unwind";
		case LogicalOpType::LOP_UNION: return "Union";
		case LogicalOpType::LOP_SINGLE_ROW: return "SingleRow";
		case LogicalOpType::LOP_CREATE_NODE: return "CreateNode";
		case LogicalOpType::LOP_CREATE_EDGE: return "CreateEdge";
		case LogicalOpType::LOP_SET: return "Set";
		case LogicalOpType::LOP_DELETE: return "Delete";
		case LogicalOpType::LOP_REMOVE: return "Remove";
		case LogicalOpType::LOP_MERGE_NODE: return "MergeNode";
		case LogicalOpType::LOP_MERGE_EDGE: return "MergeEdge";
		case LogicalOpType::LOP_CREATE_INDEX: return "CreateIndex";
		case LogicalOpType::LOP_DROP_INDEX: return "DropIndex";
		case LogicalOpType::LOP_SHOW_INDEXES: return "ShowIndexes";
		case LogicalOpType::LOP_CREATE_VECTOR_INDEX: return "CreateVectorIndex";
		case LogicalOpType::LOP_CREATE_CONSTRAINT: return "CreateConstraint";
		case LogicalOpType::LOP_DROP_CONSTRAINT: return "DropConstraint";
		case LogicalOpType::LOP_SHOW_CONSTRAINTS: return "ShowConstraints";
		case LogicalOpType::LOP_TRANSACTION_CONTROL: return "TransactionControl";
		case LogicalOpType::LOP_LIST_CONFIG: return "ListConfig";
		case LogicalOpType::LOP_SET_CONFIG: return "SetConfig";
		case LogicalOpType::LOP_CALL_PROCEDURE: return "CallProcedure";
		case LogicalOpType::LOP_ALGO_SHORTEST_PATH: return "ShortestPath";
		case LogicalOpType::LOP_VECTOR_SEARCH: return "VectorSearch";
		case LogicalOpType::LOP_TRAIN_VECTOR_INDEX: return "TrainVectorIndex";
		case LogicalOpType::LOP_EXPLAIN: return "Explain";
		case LogicalOpType::LOP_PROFILE: return "Profile";
		default: return "Unknown";
	}
}

} // namespace graph::query::logical
