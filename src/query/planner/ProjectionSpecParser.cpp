/**
 * @file ProjectionSpecParser.cpp
 * @author Nexepic
 * @date 2026/7/7
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 **/

#include "graph/query/planner/ProjectionSpecParser.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace {

using graph::PropertyType;
using graph::PropertyValue;
using graph::query::algorithm::ProjectionOrientation;
using graph::query::algorithm::ProjectionSpec;
using graph::query::algorithm::ProjectionWeightKind;
using graph::query::algorithm::ProjectionWeightSpec;
using graph::query::algorithm::RelationshipProjectionSpec;

struct ProjectionDefaults {
	ProjectionOrientation orientation = ProjectionOrientation::GPO_NATURAL;
	std::optional<ProjectionWeightSpec> weight;
};

const PropertyValue *findOption(const PropertyValue::MapType &map,
								std::initializer_list<const char *> keys) {
	for (const char *key : keys) {
		if (const auto it = map.find(key); it != map.end()) return &it->second;
	}
	return nullptr;
}

bool hasOption(const PropertyValue::MapType &map, std::initializer_list<const char *> keys) {
	return findOption(map, keys) != nullptr;
}

void rejectUnknownKeys(const PropertyValue::MapType &map,
					   const std::unordered_set<std::string> &allowed,
					   const std::string &context) {
	for (const auto &[key, _] : map) {
		if (!allowed.contains(key)) {
			throw std::runtime_error(context + " has unsupported option '" + key + "'");
		}
	}
}

std::string requireString(const PropertyValue &value, const std::string &context) {
	if (value.getType() != PropertyType::STRING) {
		throw std::runtime_error(context + " must be a string");
	}
	return std::get<std::string>(value.getVariant());
}

double requireNonNegativeFiniteNumber(const PropertyValue &value, const std::string &context) {
	double parsed = 0.0;
	if (value.getType() == PropertyType::INTEGER) {
		parsed = static_cast<double>(std::get<int64_t>(value.getVariant()));
	} else if (value.getType() == PropertyType::DOUBLE) {
		parsed = std::get<double>(value.getVariant());
	} else {
		throw std::runtime_error(context + " must be a numeric value");
	}
	if (!std::isfinite(parsed) || parsed < 0.0) {
		throw std::runtime_error(context + " must be a non-negative finite number");
	}
	return parsed;
}

bool isAllToken(const std::string &value) {
	return value.empty() || value == "*";
}

void appendToken(std::vector<std::string> &result, std::string text) {
	if (isAllToken(text)) {
		result.clear();
		return;
	}
	if (std::find(result.begin(), result.end(), text) == result.end()) {
		result.push_back(std::move(text));
	}
}

std::string toUpperAscii(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	return value;
}

ProjectionOrientation parseOrientationString(std::string value, const std::string &context) {
	value = toUpperAscii(std::move(value));
	if (value == "NATURAL" || value == "DIRECTED") {
		return ProjectionOrientation::GPO_NATURAL;
	}
	if (value == "REVERSE") {
		return ProjectionOrientation::GPO_REVERSE;
	}
	if (value == "UNDIRECTED") {
		return ProjectionOrientation::GPO_UNDIRECTED;
	}
	throw std::runtime_error(
		context + " must be one of NATURAL, REVERSE, or UNDIRECTED");
}

ProjectionOrientation parseOrientation(const PropertyValue &value, const std::string &context) {
	return parseOrientationString(requireString(value, context), context);
}

std::vector<std::string> parseProjectionTokenList(const PropertyValue &value,
												  const std::string &context) {
	std::vector<std::string> result;
	auto append = [&](const PropertyValue &item, const std::string &itemContext) {
		std::string text = requireString(item, itemContext);
		appendToken(result, std::move(text));
		return !result.empty();
	};

	if (value.getType() == PropertyType::STRING) {
		(void) append(value, context);
		return result;
	}
	if (value.getType() != PropertyType::LIST) {
		throw std::runtime_error(context + " must be a string or a list of strings");
	}
	for (const auto &item : value.getList()) {
		if (!append(item, context + " item")) break;
	}
	return result;
}

std::vector<std::string> parseStringList(const PropertyValue &value,
										 const std::string &context) {
	return parseProjectionTokenList(value, context);
}

std::vector<std::string> parseNodeProjectionMap(const PropertyValue::MapType &map,
												const std::string &context) {
	std::vector<std::string> labels;
	for (const auto &[projectionName, projectionConfig] : map) {
		if (isAllToken(projectionName)) {
			labels.clear();
			return labels;
		}

		if (projectionConfig.getType() == PropertyType::MAP) {
			const auto &config = projectionConfig.getMap();
			rejectUnknownKeys(config, {"label", "labels"}, context + " '" + projectionName + "'");
			if (const auto *label = findOption(config, {"label", "labels"})) {
				auto parsedLabels = parseProjectionTokenList(*label, context + " '" + projectionName + "'.label");
				if (parsedLabels.empty()) {
					labels.clear();
					return labels;
				}
				for (auto &parsedLabel : parsedLabels) {
					appendToken(labels, std::move(parsedLabel));
				}
			} else {
				appendToken(labels, projectionName);
			}
			continue;
		}

		if (projectionConfig.getType() == PropertyType::STRING ||
			projectionConfig.getType() == PropertyType::LIST) {
			auto parsedLabels = parseProjectionTokenList(
				projectionConfig, context + " '" + projectionName + "'");
			if (parsedLabels.empty()) {
				labels.clear();
				return labels;
			}
			for (auto &parsedLabel : parsedLabels) {
				appendToken(labels, std::move(parsedLabel));
			}
			continue;
		}

		throw std::runtime_error(
			context + " '" + projectionName + "' must be a string, list, or config map");
	}
	return labels;
}

std::vector<std::string> parseNodeProjection(const PropertyValue &value,
											 const std::string &context) {
	if (value.getType() == PropertyType::MAP) {
		return parseNodeProjectionMap(value.getMap(), context);
	}
	return parseProjectionTokenList(value, context);
}

ProjectionWeightSpec parseWeightSpecFromRelationshipMap(const PropertyValue::MapType &map,
														const std::string &context) {
	ProjectionWeightSpec weight;
	const auto *weightValue = findOption(map, {"weight"});
	const auto *weightPropertyValue = findOption(map, {"weightProperty",
													   "weight_property",
													   "relationshipWeightProperty",
													   "relationship_weight_property"});

	if (weightValue && weightPropertyValue) {
		throw std::runtime_error(context + " cannot specify both weight and weightProperty");
	}
	if (weightValue) {
		if (weightValue->getType() == PropertyType::STRING) {
			weight.kind = ProjectionWeightKind::GPWK_PROPERTY;
			weight.propertyName = requireString(*weightValue, context + ".weight");
			if (weight.propertyName.empty()) {
				throw std::runtime_error(context + ".weight must not be empty when used as a property name");
			}
		} else {
			weight.kind = ProjectionWeightKind::GPWK_CONSTANT;
			weight.constantWeight = requireNonNegativeFiniteNumber(*weightValue, context + ".weight");
		}
	}
	if (weightPropertyValue) {
		weight.kind = ProjectionWeightKind::GPWK_PROPERTY;
		weight.propertyName = requireString(*weightPropertyValue, context + ".weightProperty");
		if (weight.propertyName.empty()) {
			throw std::runtime_error(context + ".weightProperty must not be empty");
		}
	}

	if (const auto *defaultValue = findOption(map, {"defaultWeight", "default_weight", "defaultValue"})) {
		if (weight.kind != ProjectionWeightKind::GPWK_PROPERTY) {
			throw std::runtime_error(context + ".defaultWeight requires weightProperty");
		}
		weight.defaultWeight = requireNonNegativeFiniteNumber(*defaultValue, context + ".defaultWeight");
	}
	return weight;
}

std::optional<ProjectionWeightSpec> parseGlobalWeightSpec(const PropertyValue::MapType &map,
														  const std::string &context) {
	const auto *weightPropertyValue = findOption(map, {"weightProperty",
													   "weight_property",
													   "relationshipWeightProperty",
													   "relationship_weight_property"});
	const auto *defaultValue = findOption(map, {"defaultWeight", "default_weight", "defaultValue"});
	if (!weightPropertyValue && !defaultValue) {
		return std::nullopt;
	}
	if (!weightPropertyValue) {
		throw std::runtime_error(context + ".defaultWeight requires relationshipWeightProperty");
	}
	ProjectionWeightSpec weight;
	weight.kind = ProjectionWeightKind::GPWK_PROPERTY;
	weight.propertyName = requireString(*weightPropertyValue, context + ".relationshipWeightProperty");
	if (weight.propertyName.empty()) {
		throw std::runtime_error(context + ".relationshipWeightProperty must not be empty");
	}
	if (defaultValue) {
		weight.defaultWeight = requireNonNegativeFiniteNumber(*defaultValue, context + ".defaultWeight");
	}
	return weight;
}

void applyDefaults(std::vector<RelationshipProjectionSpec> &relationships,
				   const ProjectionDefaults &defaults) {
	if (!defaults.weight) return;
	for (auto &relationship : relationships) {
		if (!relationship.weight.usesWeight()) {
			relationship.weight = *defaults.weight;
		}
	}
}

RelationshipProjectionSpec parseRelationshipConfig(const std::string &type,
												   const PropertyValue &value,
												   ProjectionOrientation defaultOrientation) {
	RelationshipProjectionSpec relationship;
	relationship.type = isAllToken(type) ? "" : type;
	relationship.orientation = defaultOrientation;

	if (value.getType() == PropertyType::MAP) {
		const auto &map = value.getMap();
		rejectUnknownKeys(map, {
			"orientation",
			"type",
			"relationshipType",
			"relationship_type",
			"weight",
			"weightProperty",
			"weight_property",
			"relationshipWeightProperty",
			"relationship_weight_property",
			"defaultWeight",
			"default_weight",
			"defaultValue",
		}, "gds.graph.project relationship '" + type + "'");
		if (const auto *typeOverride = findOption(map, {"type", "relationshipType", "relationship_type"})) {
			const auto parsedType = requireString(*typeOverride, "gds.graph.project relationship '" + type + "'.type");
			relationship.type = isAllToken(parsedType) ? "" : parsedType;
		}
		if (const auto *orientation = findOption(map, {"orientation"})) {
			relationship.orientation = parseOrientation(*orientation, "gds.graph.project relationship '" + type + "'.orientation");
		}
		relationship.weight = parseWeightSpecFromRelationshipMap(
			map, "gds.graph.project relationship '" + type + "'");
		return relationship;
	}

	if (value.getType() == PropertyType::INTEGER || value.getType() == PropertyType::DOUBLE) {
		relationship.weight.kind = ProjectionWeightKind::GPWK_CONSTANT;
		relationship.weight.constantWeight = requireNonNegativeFiniteNumber(
			value, "gds.graph.project relationship '" + type + "'");
		return relationship;
	}

	throw std::runtime_error(
		"gds.graph.project relationship '" + type + "' must be a config map or numeric constant weight");
}

std::vector<RelationshipProjectionSpec>
parseRelationships(const PropertyValue &value, ProjectionOrientation defaultOrientation) {
	std::vector<RelationshipProjectionSpec> relationships;

	auto appendDefault = [&](std::string type) {
		RelationshipProjectionSpec relationship;
		relationship.type = isAllToken(type) ? "" : std::move(type);
		relationship.orientation = defaultOrientation;
		relationships.push_back(std::move(relationship));
	};

	if (value.getType() == PropertyType::STRING) {
		appendDefault(requireString(value, "gds.graph.project relationships"));
		return relationships;
	}
	if (value.getType() == PropertyType::LIST) {
		for (const auto &item : value.getList()) {
			appendDefault(requireString(item, "gds.graph.project relationships item"));
		}
		return relationships;
	}
	if (value.getType() == PropertyType::MAP) {
		const auto &map = value.getMap();
		relationships.reserve(map.size());
		for (const auto &[type, config] : map) {
			relationships.push_back(parseRelationshipConfig(type, config, defaultOrientation));
		}
		return relationships;
	}

	throw std::runtime_error(
		"gds.graph.project relationships must be a string, list, or relationship config map");
}

ProjectionSpec parseConfigMap(const std::string &name, const PropertyValue::MapType &config) {
	rejectUnknownKeys(config, {
		"nodeLabels",
		"node_labels",
		"labels",
		"relationships",
		"relationshipTypes",
		"relationship_types",
		"orientation",
		"weightProperty",
		"weight_property",
		"relationshipWeightProperty",
		"relationship_weight_property",
		"defaultWeight",
		"default_weight",
		"defaultValue",
	}, "gds.graph.project config");

	ProjectionSpec spec;
	spec.name = name;
	ProjectionDefaults defaults;

	if (const auto *orientation = findOption(config, {"orientation"})) {
		defaults.orientation = parseOrientation(*orientation, "gds.graph.project orientation");
	}
	defaults.weight = parseGlobalWeightSpec(config, "gds.graph.project config");
	spec.defaultOrientation = defaults.orientation;

	if (const auto *nodeLabels = findOption(config, {"nodeLabels", "node_labels", "labels"})) {
		spec.nodeLabels = parseNodeProjection(*nodeLabels, "gds.graph.project nodeLabels");
	}

	const bool hasRelationships = hasOption(config, {"relationships"});
	const bool hasRelationshipTypes = hasOption(config, {"relationshipTypes", "relationship_types"});
	if (hasRelationships && hasRelationshipTypes) {
		throw std::runtime_error("gds.graph.project config cannot specify both relationships and relationshipTypes");
	}
	if (const auto *relationships = findOption(config, {"relationships"})) {
		spec.relationships = parseRelationships(*relationships, defaults.orientation);
	} else if (const auto *relationshipTypes = findOption(config, {"relationshipTypes", "relationship_types"})) {
		for (auto &type : parseStringList(*relationshipTypes, "gds.graph.project relationshipTypes")) {
			RelationshipProjectionSpec relationship;
			relationship.type = std::move(type);
			relationship.orientation = defaults.orientation;
			spec.relationships.push_back(std::move(relationship));
		}
	}
	if (spec.relationships.empty()) {
		RelationshipProjectionSpec allRelationships;
		allRelationships.orientation = defaults.orientation;
		spec.relationships.push_back(std::move(allRelationships));
	}
	applyDefaults(spec.relationships, defaults);
	return spec;
}

ProjectionDefaults parseProjectionConfig(const PropertyValue &value, const std::string &context) {
	if (value.getType() != PropertyType::MAP) {
		throw std::runtime_error(context + " must be a config map");
	}
	const auto &map = value.getMap();
	rejectUnknownKeys(map, {
		"orientation",
		"weightProperty",
		"weight_property",
		"relationshipWeightProperty",
		"relationship_weight_property",
		"defaultWeight",
		"default_weight",
		"defaultValue",
	}, context);

	ProjectionDefaults defaults;
	if (const auto *orientation = findOption(map, {"orientation"})) {
		defaults.orientation = parseOrientation(*orientation, context + ".orientation");
	}
	defaults.weight = parseGlobalWeightSpec(map, context);
	return defaults;
}

ProjectionSpec parseNativeSignature(const std::string &name,
									const PropertyValue &nodeProjection,
									const PropertyValue &relationshipProjection,
									const ProjectionDefaults &defaults) {
	ProjectionSpec spec;
	spec.name = name;
	spec.defaultOrientation = defaults.orientation;
	spec.nodeLabels = parseNodeProjection(nodeProjection, "gds.graph.project node projection");
	spec.relationships = parseRelationships(relationshipProjection, defaults.orientation);
	if (spec.relationships.empty()) {
		RelationshipProjectionSpec allRelationships;
		allRelationships.orientation = defaults.orientation;
		spec.relationships.push_back(std::move(allRelationships));
	}
	applyDefaults(spec.relationships, defaults);
	return spec;
}

} // namespace

namespace graph::query::planner {

	algorithm::ProjectionSpec ProjectionSpecParser::parseGraphProjectArgs(const std::vector<PropertyValue> &args) {
		if (args.size() == 2 && args[1].getType() == PropertyType::MAP) {
			const auto name = requireString(args[0], "gds.graph.project name");
			if (name.empty()) {
				throw std::runtime_error("gds.graph.project name must not be empty");
			}
			return parseConfigMap(name, args[1].getMap());
		}

		if (args.size() < 3 || args.size() > 4) {
			throw std::runtime_error(
				"gds.graph.project expects (name, configMap), "
				"(name, nodeProjection, relationshipProjection[, configMap]), "
				"or (name, nodeLabel, relationshipType[, weightProperty])");
		}

		const auto name = requireString(args[0], "gds.graph.project name");
		if (name.empty()) {
			throw std::runtime_error("gds.graph.project name must not be empty");
		}
		if (args[1].getType() == PropertyType::STRING &&
			args[2].getType() == PropertyType::STRING &&
			(args.size() == 3 || args[3].getType() == PropertyType::STRING)) {
			const auto nodeLabel = requireString(args[1], "gds.graph.project nodeLabel");
			const auto relationshipType = requireString(args[2], "gds.graph.project relationshipType");
			const auto weightProperty = args.size() > 3
				? requireString(args[3], "gds.graph.project weightProperty")
				: std::string{};
			return algorithm::ProjectionSpec::legacy(name, nodeLabel, relationshipType, weightProperty);
		}

		const auto defaults = args.size() == 4
			? parseProjectionConfig(args[3], "gds.graph.project config")
			: ProjectionDefaults{};
		return parseNativeSignature(name, args[1], args[2], defaults);
	}

} // namespace graph::query::planner
