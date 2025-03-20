/**
 * @file Query
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Query.h"
#include "graph/core/Database.h"

namespace graph {

	std::vector<Node> Query::execute() {
		auto& storage = db.getStorage();
		std::vector<Node> results;

		for (const auto& [id, node] : storage.getNodes()) {
			if (filter(node)) {
				results.push_back(node);
			}
		}

		return results;
	}

	Query& Query::match(const std::string& label) {
		auto prevFilter = filter;
		filter = [label, prevFilter](const Node& node) {
			return node.getLabel() == label && prevFilter(node);
		};
		return *this;
	}

	Query& Query::where(const std::function<bool(const Node&)>& predicate) {
		auto prevFilter = filter;
		filter = [predicate, prevFilter](const Node& node) {
			return predicate(node) && prevFilter(node);
		};
		return *this;
	}

} // namespace graph