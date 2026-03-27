/**
 * @file UniqueConstraint.cpp
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

#include "graph/storage/constraints/UniqueConstraint.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::storage::constraints {

void UniqueConstraint::validateInsert(int64_t entityId,
	const std::unordered_map<std::string, PropertyValue> &props) {
	auto it = props.find(property_);
	if (it == props.end() || it->second.getType() == PropertyType::NULL_TYPE) {
		return; // NULL values are not considered duplicates
	}
	checkUniqueness(entityId, it->second);
}

void UniqueConstraint::validateUpdate(int64_t entityId,
	const std::unordered_map<std::string, PropertyValue> &oldProps,
	const std::unordered_map<std::string, PropertyValue> &newProps) {
	auto newIt = newProps.find(property_);
	if (newIt == newProps.end() || newIt->second.getType() == PropertyType::NULL_TYPE) {
		return; // NULL or removed — OK
	}

	// Check if value actually changed
	auto oldIt = oldProps.find(property_);
	if (oldIt != oldProps.end() && oldIt->second == newIt->second) {
		return; // Value unchanged — skip check
	}

	checkUniqueness(entityId, newIt->second);
}

void UniqueConstraint::checkUniqueness(int64_t entityId, const PropertyValue &value) const {
	auto matches = indexManager_->findNodeIdsByProperty(property_, value);
	for (int64_t matchId : matches) {
		if (matchId != entityId) {
			throw std::runtime_error(
				"Constraint violation: '" + name_ + "' - duplicate value '" +
				value.toString() + "' for property '" + property_ + "' (existing entity id: " +
				std::to_string(matchId) + ")");
		}
	}
}

} // namespace graph::storage::constraints
