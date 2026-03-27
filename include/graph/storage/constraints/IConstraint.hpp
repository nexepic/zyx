/**
 * @file IConstraint.hpp
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

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "graph/core/PropertyTypes.hpp"

namespace graph::storage::constraints {

enum class ConstraintType : uint8_t {
	CT_UNIQUE = 1,
	CT_NOT_NULL = 2,
	CT_PROPERTY_TYPE = 3,
	CT_NODE_KEY = 4
};

class IConstraint {
public:
	virtual ~IConstraint() = default;

	virtual void validateInsert(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &props) = 0;
	virtual void validateUpdate(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) = 0;

	virtual ConstraintType getType() const = 0;
	virtual std::string getName() const = 0;
	virtual std::string getLabel() const = 0;
	virtual std::vector<std::string> getProperties() const = 0;

	// For PropertyType constraint
	virtual std::string getOptions() const { return ""; }
};

} // namespace graph::storage::constraints
