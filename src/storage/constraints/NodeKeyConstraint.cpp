/**
 * @file NodeKeyConstraint.cpp
 * @author Nexepic
 * @date 2026/3/27
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/constraints/NodeKeyConstraint.hpp"
#include <algorithm>
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::storage::constraints {

void NodeKeyConstraint::validateInsert(int64_t entityId,
	const std::unordered_map<std::string, PropertyValue> &props) {
	checkNotNull(props);
	checkCompositeUniqueness(entityId, props);
}

void NodeKeyConstraint::validateUpdate(int64_t entityId,
	const std::unordered_map<std::string, PropertyValue> &oldProps,
	const std::unordered_map<std::string, PropertyValue> &newProps) {
	checkNotNull(newProps);

	// Check if composite key changed
	std::string oldKey = buildCompositeKey(oldProps);
	std::string newKey = buildCompositeKey(newProps);
	if (oldKey == newKey) return; // Unchanged

	checkCompositeUniqueness(entityId, newProps);
}

void NodeKeyConstraint::checkNotNull(
	const std::unordered_map<std::string, PropertyValue> &props) const {
	for (const auto &prop : properties_) {
		auto it = props.find(prop);
		if (it == props.end() || it->second.getType() == PropertyType::NULL_TYPE) {
			throw std::runtime_error(
				"Constraint violation: '" + name_ + "' - NODE KEY property '" +
				prop + "' must not be null");
		}
	}
}

void NodeKeyConstraint::checkCompositeUniqueness(int64_t entityId,
	const std::unordered_map<std::string, PropertyValue> &props) const {
	// Look up each property individually and intersect results
	std::vector<int64_t> candidates;
	bool first = true;

	for (const auto &prop : properties_) {
		auto it = props.find(prop);
		if (it == props.end()) return; // Missing property, can't match

		auto matches = indexManager_->findNodeIdsByProperty(prop, it->second);
		if (first) {
			candidates = std::move(matches);
			first = false;
		} else {
			// Intersect: keep only IDs present in both
			std::sort(candidates.begin(), candidates.end());
			std::sort(matches.begin(), matches.end());
			std::vector<int64_t> intersection;
			std::set_intersection(candidates.begin(), candidates.end(),
								 matches.begin(), matches.end(),
								 std::back_inserter(intersection));
			candidates = std::move(intersection);
		}

		if (candidates.empty()) return; // No matches possible
	}

	for (int64_t matchId : candidates) {
		if (matchId != entityId) {
			std::string propDesc = "(";
			for (size_t i = 0; i < properties_.size(); ++i) {
				if (i > 0) propDesc += ", ";
				propDesc += properties_[i];
			}
			propDesc += ") = (";
			for (size_t i = 0; i < properties_.size(); ++i) {
				if (i > 0) propDesc += ", ";
				auto it = props.find(properties_[i]);
				propDesc += "'" + it->second.toString() + "'";
			}
			propDesc += ")";

			throw std::runtime_error(
				"Constraint violation: '" + name_ + "' - duplicate NODE KEY " +
				propDesc + " (existing entity id: " + std::to_string(matchId) + ")");
		}
	}
}

std::string NodeKeyConstraint::buildCompositeKey(
	const std::unordered_map<std::string, PropertyValue> &props) const {
	std::string result;
	for (size_t i = 0; i < properties_.size(); ++i) {
		if (i > 0) result += "\x1F"; // Unit separator
		auto it = props.find(properties_[i]);
		if (it != props.end()) {
			result += it->second.toString();
		}
	}
	return result;
}

} // namespace graph::storage::constraints
