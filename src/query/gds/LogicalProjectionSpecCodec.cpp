/**
 * @file LogicalProjectionSpecCodec.cpp
 * @brief Strict, storage-neutral gds.graph.project argument decoder.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/gds/LogicalProjectionSpecCodec.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

namespace graph::query::gds {

	namespace {

		struct ProjectionDefaults {
			LogicalProjectionOrientation orientation = LogicalProjectionOrientation::LPO_NATURAL;
			std::optional<LogicalProjectionWeight> weight;
		};

		class ProjectionArgumentView {
		public:
			explicit ProjectionArgumentView(
					const std::span<const std::reference_wrapper<const PropertyValue>> arguments)
				: arguments_(arguments) {}
			[[nodiscard]] size_t size() const { return arguments_.size(); }
		[[nodiscard]] bool empty() const { return arguments_.empty(); }
		[[nodiscard]] const PropertyValue &operator[](const size_t index) const { return arguments_[index].get(); }
		[[nodiscard]] const PropertyValue &front() const { return arguments_.front().get(); }

		private:
			std::span<const std::reference_wrapper<const PropertyValue>> arguments_;
		};

		[[nodiscard]] std::string requireText(const PropertyValue &value, const std::string_view context,
				const LogicalProjectionSpecCodecBounds &bounds, const bool allowEmpty = false) {
			if (value.getType() != PropertyType::STRING) {
				throw std::invalid_argument(std::string(context) + " must be a string");
			}
			const std::string &text = std::get<std::string>(value.getVariant());
			if ((!allowEmpty && text.empty()) || text.size() > bounds.maximum_text_bytes) {
				throw std::invalid_argument(std::string(context) + " has an invalid text length");
			}
			return text;
		}

		void requireMapBound(const PropertyValue::MapType &map, const std::string_view context,
				const LogicalProjectionSpecCodecBounds &bounds) {
			if (map.size() > bounds.maximum_map_entries) {
				throw std::invalid_argument(std::string(context) + " exceeds the map-entry bound");
			}
		}

		void requireListBound(const std::vector<PropertyValue> &list, const std::string_view context,
				const LogicalProjectionSpecCodecBounds &bounds) {
			if (list.size() > bounds.maximum_list_entries) {
				throw std::invalid_argument(std::string(context) + " exceeds the list-entry bound");
			}
		}

		void rejectUnknownKeys(const PropertyValue::MapType &map, const std::unordered_set<std::string_view> &allowed,
				const std::string_view context, const LogicalProjectionSpecCodecBounds &bounds) {
			requireMapBound(map, context, bounds);
			for (const auto &[key, _]: map) {
				if (key.size() > bounds.maximum_text_bytes || !allowed.contains(key)) {
					throw std::invalid_argument(std::string(context) + " has unsupported option '" + key + "'");
				}
			}
		}

		[[nodiscard]] const PropertyValue *findUniqueOption(const PropertyValue::MapType &map,
				const std::initializer_list<std::string_view> aliases, const std::string_view context) {
			const PropertyValue *result = nullptr;
			for (const std::string_view alias: aliases) {
				const auto found = map.find(std::string(alias));
				if (found == map.end()) {
					continue;
				}
				if (result != nullptr) {
					throw std::invalid_argument(std::string(context) + " specifies the same option more than once");
				}
				result = &found->second;
			}
			return result;
		}

		[[nodiscard]] double requireNonNegativeFiniteNumber(const PropertyValue &value, const std::string_view context) {
			double parsed = 0.0;
			if (value.getType() == PropertyType::INTEGER) {
				parsed = static_cast<double>(std::get<int64_t>(value.getVariant()));
			} else if (value.getType() == PropertyType::DOUBLE) {
				parsed = std::get<double>(value.getVariant());
			} else {
				throw std::invalid_argument(std::string(context) + " must be numeric");
			}
			if (!std::isfinite(parsed) || parsed < 0.0) {
				throw std::invalid_argument(std::string(context) + " must be a non-negative finite number");
			}
			return parsed;
		}

		[[nodiscard]] bool isAllToken(const std::string_view text) noexcept { return text.empty() || text == "*"; }

		[[nodiscard]] std::string uppercaseAscii(std::string text) {
			std::ranges::transform(text, text.begin(), [](const unsigned char character) {
				return static_cast<char>(std::toupper(character));
			});
			return text;
		}

		[[nodiscard]] LogicalProjectionOrientation decodeOrientation(const PropertyValue &value,
				const std::string_view context, const LogicalProjectionSpecCodecBounds &bounds) {
			const std::string text = uppercaseAscii(requireText(value, context, bounds));
			if (text == "NATURAL" || text == "DIRECTED") {
				return LogicalProjectionOrientation::LPO_NATURAL;
			}
			if (text == "REVERSE") {
				return LogicalProjectionOrientation::LPO_REVERSE;
			}
			if (text == "UNDIRECTED") {
				return LogicalProjectionOrientation::LPO_UNDIRECTED;
			}
			throw std::invalid_argument(std::string(context) + " must be NATURAL, REVERSE, or UNDIRECTED");
		}

		void appendToken(std::vector<std::string> &tokens, std::string token) {
			if (isAllToken(token)) {
				tokens.clear();
				return;
			}
			if (std::ranges::find(tokens, token) == tokens.end()) {
				tokens.push_back(std::move(token));
			}
		}

		[[nodiscard]] std::vector<std::string> decodeTokens(const PropertyValue &value, const std::string_view context,
				const LogicalProjectionSpecCodecBounds &bounds) {
			std::vector<std::string> result;
			if (value.getType() == PropertyType::STRING) {
				appendToken(result, requireText(value, context, bounds, true));
				return result;
			}
			if (value.getType() != PropertyType::LIST) {
				throw std::invalid_argument(std::string(context) + " must be a string or a list of strings");
			}
			const std::vector<PropertyValue> &list = value.getList();
			requireListBound(list, context, bounds);
			for (const PropertyValue &item: list) {
				appendToken(result, requireText(item, std::string(context) + " item", bounds, true));
				if (result.empty()) {
					return result;
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<std::string> decodeNodeProjection(const PropertyValue &value,
				const LogicalProjectionSpecCodecBounds &bounds) {
			if (value.getType() != PropertyType::MAP) {
				return decodeTokens(value, "gds.graph.project node projection", bounds);
			}
			const PropertyValue::MapType &map = value.getMap();
			requireMapBound(map, "gds.graph.project node projection", bounds);
			std::vector<std::string> labels;
			for (const auto &[alias, config]: map) {
				if (alias.size() > bounds.maximum_text_bytes || isAllToken(alias)) {
					labels.clear();
					return labels;
				}
				if (config.getType() == PropertyType::MAP) {
					const PropertyValue::MapType &entry = config.getMap();
					rejectUnknownKeys(entry, {"label", "labels"}, "gds.graph.project node projection entry", bounds);
					if (const PropertyValue *labelsValue = findUniqueOption(entry, {"label", "labels"},
							"gds.graph.project node projection entry")) {
						for (std::string label: decodeTokens(*labelsValue, "gds.graph.project node projection label", bounds)) {
							appendToken(labels, std::move(label));
						}
					} else {
						appendToken(labels, alias);
					}
				} else {
					for (std::string label: decodeTokens(config, "gds.graph.project node projection entry", bounds)) {
						appendToken(labels, std::move(label));
					}
				}
				if (labels.empty()) {
					return labels;
				}
			}
			return labels;
		}

		[[nodiscard]] LogicalProjectionWeight decodeRelationshipWeight(const PropertyValue::MapType &map,
				const std::string_view context, const LogicalProjectionSpecCodecBounds &bounds) {
			const PropertyValue *weight = findUniqueOption(map, {"weight"}, context);
			const PropertyValue *property = findUniqueOption(map,
					{"weightProperty", "weight_property", "relationshipWeightProperty", "relationship_weight_property"}, context);
			if (weight != nullptr && property != nullptr) {
				throw std::invalid_argument(std::string(context) + " cannot specify both weight and weightProperty");
			}
			LogicalProjectionWeight result;
			if (weight != nullptr) {
				if (weight->getType() == PropertyType::STRING) {
					result.kind = LogicalProjectionWeightKind::LPWK_PROPERTY;
					result.property_name = requireText(*weight, std::string(context) + ".weight", bounds);
				} else {
					result.kind = LogicalProjectionWeightKind::LPWK_CONSTANT;
					result.constant_weight = requireNonNegativeFiniteNumber(*weight, std::string(context) + ".weight");
				}
			}
			if (property != nullptr) {
				result.kind = LogicalProjectionWeightKind::LPWK_PROPERTY;
				result.property_name = requireText(*property, std::string(context) + ".weightProperty", bounds);
			}
			if (const PropertyValue *defaultWeight = findUniqueOption(map,
					{"defaultWeight", "default_weight", "defaultValue"}, context)) {
				if (result.kind != LogicalProjectionWeightKind::LPWK_PROPERTY) {
					throw std::invalid_argument(std::string(context) + ".defaultWeight requires a property weight");
				}
				result.default_weight = requireNonNegativeFiniteNumber(*defaultWeight, std::string(context) + ".defaultWeight");
			}
			return result;
		}

		[[nodiscard]] std::optional<LogicalProjectionWeight> decodeGlobalWeight(const PropertyValue::MapType &map,
				const std::string_view context, const LogicalProjectionSpecCodecBounds &bounds) {
			const PropertyValue *property = findUniqueOption(map,
					{"weightProperty", "weight_property", "relationshipWeightProperty", "relationship_weight_property"}, context);
			const PropertyValue *defaultWeight = findUniqueOption(map,
					{"defaultWeight", "default_weight", "defaultValue"}, context);
			if (property == nullptr && defaultWeight == nullptr) {
				return std::nullopt;
			}
			if (property == nullptr) {
				throw std::invalid_argument(std::string(context) + ".defaultWeight requires relationshipWeightProperty");
			}
			LogicalProjectionWeight result;
			result.kind = LogicalProjectionWeightKind::LPWK_PROPERTY;
			result.property_name = requireText(*property, std::string(context) + ".relationshipWeightProperty", bounds);
			if (defaultWeight != nullptr) {
				result.default_weight = requireNonNegativeFiniteNumber(*defaultWeight, std::string(context) + ".defaultWeight");
			}
			return result;
		}

		[[nodiscard]] LogicalRelationshipProjection decodeRelationshipEntry(std::string type, const PropertyValue &value,
				const LogicalProjectionOrientation defaultOrientation, const LogicalProjectionSpecCodecBounds &bounds) {
			LogicalRelationshipProjection result;
			result.type = isAllToken(type) ? "" : std::move(type);
			result.orientation = defaultOrientation;
			if (value.getType() == PropertyType::INTEGER || value.getType() == PropertyType::DOUBLE) {
				result.weight.kind = LogicalProjectionWeightKind::LPWK_CONSTANT;
				result.weight.constant_weight = requireNonNegativeFiniteNumber(value, "gds.graph.project relationship weight");
				return result;
			}
			if (value.getType() != PropertyType::MAP) {
				throw std::invalid_argument("gds.graph.project relationship config must be a map or numeric weight");
			}
			const PropertyValue::MapType &map = value.getMap();
			rejectUnknownKeys(map, {"orientation", "type", "relationshipType", "relationship_type", "weight",
					"weightProperty", "weight_property", "relationshipWeightProperty", "relationship_weight_property",
					"defaultWeight", "default_weight", "defaultValue"}, "gds.graph.project relationship config", bounds);
			if (const PropertyValue *typeOverride = findUniqueOption(map, {"type", "relationshipType", "relationship_type"},
					"gds.graph.project relationship config")) {
				result.type = requireText(*typeOverride, "gds.graph.project relationship type", bounds, true);
				if (isAllToken(result.type)) {
					result.type.clear();
				}
			}
			if (const PropertyValue *orientation = findUniqueOption(map, {"orientation"},
					"gds.graph.project relationship config")) {
				result.orientation = decodeOrientation(*orientation, "gds.graph.project relationship orientation", bounds);
			}
			result.weight = decodeRelationshipWeight(map, "gds.graph.project relationship config", bounds);
			return result;
		}

		[[nodiscard]] std::vector<LogicalRelationshipProjection> decodeRelationships(const PropertyValue &value,
				const LogicalProjectionOrientation defaultOrientation, const LogicalProjectionSpecCodecBounds &bounds) {
			std::vector<LogicalRelationshipProjection> relationships;
			const auto appendDefault = [&relationships, defaultOrientation](std::string type) {
				relationships.push_back({.type = isAllToken(type) ? "" : std::move(type), .orientation = defaultOrientation});
			};
			if (value.getType() == PropertyType::STRING) {
				appendDefault(requireText(value, "gds.graph.project relationships", bounds, true));
				return relationships;
			}
			if (value.getType() == PropertyType::LIST) {
				const std::vector<PropertyValue> &list = value.getList();
				requireListBound(list, "gds.graph.project relationships", bounds);
				for (const PropertyValue &item: list) {
					appendDefault(requireText(item, "gds.graph.project relationship", bounds, true));
				}
				return relationships;
			}
			if (value.getType() != PropertyType::MAP) {
				throw std::invalid_argument("gds.graph.project relationships must be a string, list, or map");
			}
			const PropertyValue::MapType &map = value.getMap();
			requireMapBound(map, "gds.graph.project relationships", bounds);
			relationships.reserve(map.size());
			for (const auto &[type, config]: map) {
				if (type.size() > bounds.maximum_text_bytes) {
					throw std::invalid_argument("gds.graph.project relationship type has an invalid text length");
				}
				relationships.push_back(decodeRelationshipEntry(type, config, defaultOrientation, bounds));
			}
			return relationships;
		}

		void applyDefaults(std::vector<LogicalRelationshipProjection> &relationships,
				const ProjectionDefaults &defaults) {
			if (!defaults.weight.has_value()) {
				return;
			}
			for (LogicalRelationshipProjection &relationship: relationships) {
				if (relationship.weight.kind == LogicalProjectionWeightKind::LPWK_NONE) {
					relationship.weight = *defaults.weight;
				}
			}
		}

		[[nodiscard]] ProjectionDefaults decodeDefaults(const PropertyValue &value, const std::string_view context,
				const LogicalProjectionSpecCodecBounds &bounds) {
			if (value.getType() != PropertyType::MAP) {
				throw std::invalid_argument(std::string(context) + " must be a map");
			}
			const PropertyValue::MapType &map = value.getMap();
			rejectUnknownKeys(map, {"orientation", "weightProperty", "weight_property", "relationshipWeightProperty",
					"relationship_weight_property", "defaultWeight", "default_weight", "defaultValue"}, context, bounds);
			ProjectionDefaults result;
			if (const PropertyValue *orientation = findUniqueOption(map, {"orientation"}, context)) {
				result.orientation = decodeOrientation(*orientation, std::string(context) + ".orientation", bounds);
			}
			result.weight = decodeGlobalWeight(map, context, bounds);
			return result;
		}

		[[nodiscard]] LogicalGraphProjectionSpec decodeConfigSignature(const std::string &name,
				const PropertyValue::MapType &map, const LogicalProjectionSpecCodecBounds &bounds) {
			rejectUnknownKeys(map, {"nodeLabels", "node_labels", "labels", "relationships", "relationshipTypes",
					"relationship_types", "orientation", "weightProperty", "weight_property", "relationshipWeightProperty",
					"relationship_weight_property", "defaultWeight", "default_weight", "defaultValue"},
					"gds.graph.project config", bounds);
			LogicalGraphProjectionSpec result;
			result.name = name;
			ProjectionDefaults defaults;
			if (const PropertyValue *orientation = findUniqueOption(map, {"orientation"},
					"gds.graph.project config")) {
				defaults.orientation = decodeOrientation(*orientation, "gds.graph.project config.orientation", bounds);
			}
			defaults.weight = decodeGlobalWeight(map, "gds.graph.project config", bounds);
			result.default_orientation = defaults.orientation;
			if (const PropertyValue *labels = findUniqueOption(map, {"nodeLabels", "node_labels", "labels"},
					"gds.graph.project config")) {
				result.node_labels = decodeNodeProjection(*labels, bounds);
			}
			const PropertyValue *relationships = findUniqueOption(map, {"relationships"}, "gds.graph.project config");
			const PropertyValue *relationshipTypes = findUniqueOption(map, {"relationshipTypes", "relationship_types"},
					"gds.graph.project config");
			if (relationships != nullptr && relationshipTypes != nullptr) {
				throw std::invalid_argument("gds.graph.project config cannot specify both relationships and relationshipTypes");
			}
			if (relationships != nullptr) {
				result.relationships = decodeRelationships(*relationships, defaults.orientation, bounds);
			} else if (relationshipTypes != nullptr) {
				for (std::string type: decodeTokens(*relationshipTypes, "gds.graph.project relationshipTypes", bounds)) {
					result.relationships.push_back({.type = std::move(type), .orientation = defaults.orientation});
				}
			}
			if (result.relationships.empty()) {
				result.relationships.push_back({.orientation = defaults.orientation});
			}
			applyDefaults(result.relationships, defaults);
			return result;
		}

	} // namespace

	LogicalGraphProjectionSpec LogicalProjectionSpecCodec::decodeGraphProjectArguments(
			const std::vector<PropertyValue> &arguments, const LogicalProjectionSpecCodecBounds bounds) {
		std::vector<std::reference_wrapper<const PropertyValue>> references;
		references.reserve(std::min(arguments.size(), bounds.maximum_arguments));
		if (arguments.size() > bounds.maximum_arguments) {
			throw std::invalid_argument("gds.graph.project codec received invalid argument bounds");
		}
		for (const PropertyValue &argument: arguments) references.emplace_back(argument);
		return decodeGraphProjectArguments(std::span<const std::reference_wrapper<const PropertyValue>>(references), bounds);
	}

	LogicalGraphProjectionSpec LogicalProjectionSpecCodec::decodeGraphProjectArguments(
			const std::span<const std::reference_wrapper<const PropertyValue>> arguments,
			const LogicalProjectionSpecCodecBounds bounds) {
		const ProjectionArgumentView view(arguments);
		if (bounds.maximum_arguments < 4U || bounds.maximum_map_entries == 0U || bounds.maximum_list_entries == 0U ||
			bounds.maximum_text_bytes == 0U || view.empty() || view.size() > bounds.maximum_arguments) {
			throw std::invalid_argument("gds.graph.project codec received invalid argument bounds");
		}
		const std::string name = requireText(view.front(), "gds.graph.project name", bounds);
		if (view.size() == 2U && view[1].getType() == PropertyType::MAP) {
			return decodeConfigSignature(name, view[1].getMap(), bounds);
		}
		if (view.size() < 3U || view.size() > 4U) {
			throw std::invalid_argument("gds.graph.project expects (name, configMap), (name, nodeProjection, relationshipProjection[, configMap]), or (name, nodeLabel, relationshipType[, weightProperty])");
		}

		LogicalGraphProjectionSpec result;
		result.name = name;
		if (view[1].getType() == PropertyType::STRING && view[2].getType() == PropertyType::STRING &&
			(view.size() == 3U || view[3].getType() == PropertyType::STRING)) {
			const std::string nodeLabel = requireText(view[1], "gds.graph.project nodeLabel", bounds, true);
			const std::string relationshipType = requireText(view[2], "gds.graph.project relationshipType", bounds, true);
			result.node_labels = isAllToken(nodeLabel) ? std::vector<std::string>{} : std::vector<std::string>{nodeLabel};
			LogicalRelationshipProjection relationship;
			relationship.type = isAllToken(relationshipType) ? "" : relationshipType;
			if (view.size() == 4U) {
				relationship.weight.kind = LogicalProjectionWeightKind::LPWK_PROPERTY;
				relationship.weight.property_name = requireText(view[3], "gds.graph.project weightProperty", bounds, true);
				if (relationship.weight.property_name.empty()) {
					relationship.weight = {};
				}
			}
			result.relationships.push_back(std::move(relationship));
			return result;
		}

		const ProjectionDefaults defaults = view.size() == 4U
				? decodeDefaults(view[3], "gds.graph.project config", bounds)
				: ProjectionDefaults{};
		result.default_orientation = defaults.orientation;
		result.node_labels = decodeNodeProjection(view[1], bounds);
		result.relationships = decodeRelationships(view[2], defaults.orientation, bounds);
		if (result.relationships.empty()) {
			result.relationships.push_back({.orientation = defaults.orientation});
		}
		applyDefaults(result.relationships, defaults);
		return result;
	}

} // namespace graph::query::gds
