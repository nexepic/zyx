/**
 * @file ProjectionSpec.hpp
 * @author Nexepic
 * @date 2026/7/7
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
#include <vector>

namespace graph::query::algorithm {

	enum class ProjectionOrientation {
		GPO_NATURAL,
		GPO_REVERSE,
		GPO_UNDIRECTED,
	};

	enum class ProjectionWeightKind {
		GPWK_NONE,
		GPWK_CONSTANT,
		GPWK_PROPERTY,
	};

	struct ProjectionWeightSpec {
		ProjectionWeightKind kind = ProjectionWeightKind::GPWK_NONE;
		double constantWeight = 1.0;
		std::string propertyName;
		double defaultWeight = 1.0;

		[[nodiscard]] bool usesWeight() const noexcept {
			return kind != ProjectionWeightKind::GPWK_NONE;
		}
	};

	struct RelationshipProjectionSpec {
		/// Empty type means all relationship types.
		std::string type;
		ProjectionOrientation orientation = ProjectionOrientation::GPO_NATURAL;
		ProjectionWeightSpec weight;
	};

	struct ProjectionSpec {
		std::string name;
		/// Empty labels means all node labels.
		std::vector<std::string> nodeLabels;
		/// Empty relationship specs means all relationship types with defaults.
		std::vector<RelationshipProjectionSpec> relationships;
		ProjectionOrientation defaultOrientation = ProjectionOrientation::GPO_NATURAL;

		[[nodiscard]] bool usesWeights() const noexcept {
			for (const auto &relationship : relationships) {
				if (relationship.weight.usesWeight()) return true;
			}
			return false;
		}

		[[nodiscard]] bool includesAllNodeLabels() const noexcept {
			return nodeLabels.empty();
		}

		static ProjectionSpec legacy(std::string name,
									 std::string nodeLabel,
									 std::string relationshipType,
									 std::string weightProperty);
	};

} // namespace graph::query::algorithm
