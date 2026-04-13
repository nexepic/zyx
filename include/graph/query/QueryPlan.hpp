/**
 * @file QueryPlan.hpp
 * @brief Encapsulates a logical plan with metadata flags.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::query {

/**
 * @struct QueryPlan
 * @brief A logical plan root plus build-time metadata flags.
 *
 * The flags are accumulated during plan construction and allow O(1)
 * read-only / schema-write checks without walking the tree.
 */
struct QueryPlan {
	std::unique_ptr<logical::LogicalOperator> root;
	bool mutatesData = false;   // CREATE, SET, DELETE, MERGE, REMOVE, FOREACH, LOAD CSV
	bool mutatesSchema = false; // CREATE INDEX, DROP INDEX, CREATE/DROP CONSTRAINT, etc.
	bool cacheable = true;      // False for DDL, transaction control, EXPLAIN, PROFILE
	bool optimizable = true;    // False for DDL, transaction control, CALL PROCEDURE, EXPLAIN, PROFILE

	/**
	 * @brief Clone the plan (deep-copies the root, copies flags).
	 */
	[[nodiscard]] QueryPlan clone() const {
		QueryPlan copy;
		if (root) {
			copy.root = root->clone();
		}
		copy.mutatesData = mutatesData;
		copy.mutatesSchema = mutatesSchema;
		copy.cacheable = cacheable;
		copy.optimizable = optimizable;
		return copy;
	}
};

} // namespace graph::query
