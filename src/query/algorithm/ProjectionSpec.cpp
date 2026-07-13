/**
 * @file ProjectionSpec.cpp
 * @author Nexepic
 * @date 2026/7/7
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 **/

#include "graph/query/algorithm/ProjectionSpec.hpp"

#include <utility>

namespace graph::query::algorithm {

	ProjectionSpec ProjectionSpec::legacy(std::string name,
										  std::string nodeLabel,
										  std::string relationshipType,
										  std::string weightProperty) {
		ProjectionSpec spec;
		spec.name = std::move(name);
		if (!nodeLabel.empty()) {
			spec.nodeLabels.push_back(std::move(nodeLabel));
		}

		RelationshipProjectionSpec relationship;
		relationship.type = std::move(relationshipType);
		if (!weightProperty.empty()) {
			relationship.weight.kind = ProjectionWeightKind::GPWK_PROPERTY;
			relationship.weight.propertyName = std::move(weightProperty);
			relationship.weight.defaultWeight = 1.0;
		}
		spec.relationships.push_back(std::move(relationship));
		return spec;
	}

} // namespace graph::query::algorithm
