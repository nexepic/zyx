/**
 * @file IEntityValidator.hpp
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

#include <string>
#include <unordered_map>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"

namespace graph::storage::constraints {

class IEntityValidator {
public:
	virtual ~IEntityValidator() = default;

	virtual void validateNodeInsert([[maybe_unused]] const Node &node,
		[[maybe_unused]] const std::unordered_map<std::string, PropertyValue> &props) {}
	virtual void validateNodeUpdate([[maybe_unused]] const Node &node,
		[[maybe_unused]] const std::unordered_map<std::string, PropertyValue> &oldProps,
		[[maybe_unused]] const std::unordered_map<std::string, PropertyValue> &newProps) {}
	virtual void validateNodeDelete([[maybe_unused]] const Node &node) {}

	virtual void validateEdgeInsert([[maybe_unused]] const Edge &edge,
		[[maybe_unused]] const std::unordered_map<std::string, PropertyValue> &props) {}
	virtual void validateEdgeUpdate([[maybe_unused]] const Edge &edge,
		[[maybe_unused]] const std::unordered_map<std::string, PropertyValue> &oldProps,
		[[maybe_unused]] const std::unordered_map<std::string, PropertyValue> &newProps) {}
	virtual void validateEdgeDelete([[maybe_unused]] const Edge &edge) {}
};

} // namespace graph::storage::constraints
