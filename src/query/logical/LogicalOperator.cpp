/**
 * @file LogicalOperator.cpp
 * @brief Free-function implementations for LogicalOpType utilities.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::query::logical {

	bool isDataMutatingOp(LogicalOpType type) {
		switch (type) {
			case LogicalOpType::LOP_CREATE_NODE:
			case LogicalOpType::LOP_CREATE_EDGE:
			case LogicalOpType::LOP_SET:
			case LogicalOpType::LOP_DELETE:
			case LogicalOpType::LOP_REMOVE:
			case LogicalOpType::LOP_MERGE_NODE:
			case LogicalOpType::LOP_MERGE_EDGE:
			case LogicalOpType::LOP_FOREACH:
			case LogicalOpType::LOP_LOAD_CSV:
			case LogicalOpType::LOP_SET_CONFIG:
			case LogicalOpType::LOP_TRAIN_VECTOR_INDEX:
				return true;
			default:
				return false;
		}
	}

	bool isSchemaMutatingOp(LogicalOpType type) {
		switch (type) {
			case LogicalOpType::LOP_CREATE_INDEX:
			case LogicalOpType::LOP_DROP_INDEX:
			case LogicalOpType::LOP_CREATE_VECTOR_INDEX:
			case LogicalOpType::LOP_CREATE_CONSTRAINT:
			case LogicalOpType::LOP_DROP_CONSTRAINT:
				return true;
			default:
				return false;
		}
	}

	std::string toString(LogicalOpType type) {
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
			case LogicalOpType::LOP_FOREACH: return "Foreach";
			case LogicalOpType::LOP_CALL_SUBQUERY: return "CallSubquery";
			case LogicalOpType::LOP_LOAD_CSV: return "LoadCsv";
			case LogicalOpType::LOP_NAMED_PATH: return "NamedPath";
			default: return "Unknown";
		}
	}

} // namespace graph::query::logical
