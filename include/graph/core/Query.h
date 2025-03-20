/**
 * @file Query.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <vector>
#include <functional>
#include "Node.h"
#include "graph/core/Types.h"

namespace graph {

	class Database;

	class Query {
	public:
		explicit Query(Database& db) : db(db) {}

		std::vector<Node> execute();
		Query& match(const std::string& label);
		Query& where(const std::function<bool(const Node&)>& predicate);
		Query &traverse(Direction direction);

	private:
		Database& db;
		std::function<bool(const Node&)> filter = [](const Node&) { return true; };

	};

} // namespace graph