/**
 * @file LogicalPlanSchemaRuntime.cpp
 * @brief Implementation of the fail-closed logical DDL runtime.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/LogicalPlanSchemaRuntime.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalCreateConstraint.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateVectorIndex.hpp"
#include "graph/query/logical/operators/LogicalDropConstraint.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalShowConstraints.hpp"
#include "graph/query/logical/operators/LogicalShowIndexes.hpp"

namespace graph::query::execution {

	namespace {

		[[nodiscard]] std::string indexKindName(storage::LogicalGraphIndexKind kind) {
			switch (kind) {
				case storage::LogicalGraphIndexKind::LGIK_PROPERTY: return "property";
				case storage::LogicalGraphIndexKind::LGIK_COMPOSITE: return "composite";
				case storage::LogicalGraphIndexKind::LGIK_VECTOR: return "vector";
			}
			throw std::runtime_error("logical schema runtime received an unknown Index kind");
		}

		[[nodiscard]] std::string joinPropertyKeys(const std::vector<std::string> &keys) {
			std::string result;
			for (size_t index = 0U; index < keys.size(); ++index) {
				if (index != 0U) {
					result.push_back(',');
				}
				result += keys[index];
			}
			return result;
		}

		[[nodiscard]] storage::LogicalGraphConstraintEntityKind constraintEntityKind(std::string_view value) {
			if (value == "node") return storage::LogicalGraphConstraintEntityKind::LGCEK_NODE;
			if (value == "edge") return storage::LogicalGraphConstraintEntityKind::LGCEK_EDGE;
			throw std::logic_error("logical CREATE CONSTRAINT has an unknown entity kind");
		}

		[[nodiscard]] storage::LogicalGraphConstraintKind constraintKind(std::string_view value) {
			if (value == "unique") return storage::LogicalGraphConstraintKind::LGCK_UNIQUE;
			if (value == "not_null") return storage::LogicalGraphConstraintKind::LGCK_NOT_NULL;
			if (value == "property_type") return storage::LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE;
			if (value == "node_key") return storage::LogicalGraphConstraintKind::LGCK_NODE_KEY;
			throw std::logic_error("logical CREATE CONSTRAINT has an unknown constraint kind");
		}

		[[nodiscard]] storage::LogicalGraphConstraintPropertyType constraintPropertyType(std::string_view value) {
			if (value.empty()) return storage::LogicalGraphConstraintPropertyType::LGCPT_NONE;
			if (value == "BOOLEAN") return storage::LogicalGraphConstraintPropertyType::LGCPT_BOOLEAN;
			if (value == "INTEGER") return storage::LogicalGraphConstraintPropertyType::LGCPT_INTEGER;
			if (value == "FLOAT" || value == "DOUBLE") return storage::LogicalGraphConstraintPropertyType::LGCPT_FLOAT;
			if (value == "STRING") return storage::LogicalGraphConstraintPropertyType::LGCPT_STRING;
			if (value == "LIST") return storage::LogicalGraphConstraintPropertyType::LGCPT_LIST;
			if (value == "MAP") return storage::LogicalGraphConstraintPropertyType::LGCPT_MAP;
			throw std::logic_error("logical CREATE CONSTRAINT has an unknown Property type");
		}

		[[nodiscard]] std::string constraintKindName(storage::LogicalGraphConstraintKind kind) {
			switch (kind) {
				case storage::LogicalGraphConstraintKind::LGCK_UNIQUE: return "unique";
				case storage::LogicalGraphConstraintKind::LGCK_NOT_NULL: return "not_null";
				case storage::LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE: return "property_type";
				case storage::LogicalGraphConstraintKind::LGCK_NODE_KEY: return "node_key";
			}
			throw std::runtime_error("logical schema runtime received an unknown constraint kind");
		}

		[[nodiscard]] std::string constraintEntityKindName(storage::LogicalGraphConstraintEntityKind kind) {
			switch (kind) {
				case storage::LogicalGraphConstraintEntityKind::LGCEK_NODE: return "node";
				case storage::LogicalGraphConstraintEntityKind::LGCEK_EDGE: return "edge";
			}
			throw std::runtime_error("logical schema runtime received an unknown constraint entity kind");
		}

	} // namespace

	QueryResult LogicalPlanSchemaRuntime::execute(const logical::LogicalOperator &root, size_t maximumIndexes,
																			 const QueryContext &context) const {
		if (maximumIndexes == 0U) {
			throw std::invalid_argument("logical schema runtime requires a positive Index result bound");
		}
		context.checkGuard();
		if (root.getType() == logical::LogicalOpType::LOP_CREATE_INDEX) {
			const auto &create = dynamic_cast<const logical::LogicalCreateIndex &>(root);
			if (create.getLabel().empty() || create.getPropertyKeys().empty()) {
				throw std::logic_error("logical CREATE INDEX has an incomplete node Property schema");
			}
			const storage::LogicalGraphIndexDefinition definition = backend_.createLogicalNodeIndex(
					{create.getIndexName(), create.getLabel(), create.getPropertyKeys()});
			if (definition.index_id == 0U || definition.name.empty()) {
				throw std::logic_error("logical CREATE INDEX backend returned an invalid durable definition");
			}
			QueryResult result;
			result.setColumns({"result"});
			result.addRow({{"result", PropertyValue("Index created")}});
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_CREATE_VECTOR_INDEX) {
			const auto &create = dynamic_cast<const logical::LogicalCreateVectorIndex &>(root);
			if (create.getLabel().empty() || create.getProperty().empty() ||
				create.getDimension() <= 0 ||
				static_cast<uint64_t>(create.getDimension()) > std::numeric_limits<uint32_t>::max() ||
				create.getMetric().empty()) {
				throw std::logic_error("logical CREATE VECTOR INDEX has an incomplete or invalid vector schema");
			}
			const storage::LogicalGraphIndexDefinition definition = backend_.createLogicalNodeVectorIndex(
					{create.getIndexName(), create.getLabel(), create.getProperty(),
					 static_cast<uint32_t>(create.getDimension()), create.getMetric()});
			if (definition.index_id == 0U || definition.name.empty() ||
				definition.kind != storage::LogicalGraphIndexKind::LGIK_VECTOR) {
				throw std::logic_error("logical CREATE VECTOR INDEX backend returned an invalid durable definition");
			}
			QueryResult result;
			result.setColumns({"result"});
			result.addRow({{"result", PropertyValue("Index created")}});
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_DROP_INDEX) {
			const auto &drop = dynamic_cast<const logical::LogicalDropIndex &>(root);
			const bool dropped = !drop.getIndexName().empty()
									 ? backend_.dropLogicalIndexByName(drop.getIndexName())
									 : backend_.dropLogicalNodeIndexBySchema(drop.getLabel(), drop.getPropertyKey());
			if (!dropped) {
				return {};
			}
			QueryResult result;
			result.setColumns({"result"});
			result.addRow({{"result", PropertyValue("Index dropped")}});
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_SHOW_INDEXES) {
			const auto &show = dynamic_cast<const logical::LogicalShowIndexes &>(root);
			static_cast<void>(show);
			const std::vector<storage::LogicalGraphIndexDefinition> definitions = backend_.listLogicalIndexes(maximumIndexes);
			QueryResult result;
			result.setColumns({"name", "type", "label", "properties"});
			for (const storage::LogicalGraphIndexDefinition &definition: definitions) {
				context.checkGuard();
				result.addRow({{"name", PropertyValue(definition.name)},
							   {"type", PropertyValue(indexKindName(definition.kind))},
							   {"label", PropertyValue(definition.label)},
							   {"properties", PropertyValue(joinPropertyKeys(definition.property_keys))}});
			}
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_CREATE_CONSTRAINT) {
			const auto &create = dynamic_cast<const logical::LogicalCreateConstraint &>(root);
			if (create.getName().empty() || create.getLabel().empty() || create.getProperties().empty()) {
				throw std::logic_error("logical CREATE CONSTRAINT has an incomplete schema");
			}
			const storage::LogicalGraphConstraintDefinition definition = backend_.createLogicalConstraint(
					{create.getName(), constraintEntityKind(create.getEntityType()),
					 constraintKind(create.getConstraintType()), create.getLabel(), create.getProperties(),
					 constraintPropertyType(create.getOptions())});
			if (definition.constraint_id == 0U || definition.name.empty()) {
				throw std::logic_error("logical CREATE CONSTRAINT backend returned an invalid durable definition");
			}
			QueryResult result;
			result.setColumns({"result"});
			result.addRow({{"result", PropertyValue("Constraint created")}});
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_DROP_CONSTRAINT) {
			const auto &drop = dynamic_cast<const logical::LogicalDropConstraint &>(root);
			const bool dropped = backend_.dropLogicalConstraintByName(drop.getName());
			if (!dropped) {
				if (!drop.isIfExists()) {
					throw std::runtime_error("Constraint '" + drop.getName() + "' does not exist");
				}
				return {};
			}
			QueryResult result;
			result.setColumns({"result"});
			result.addRow({{"result", PropertyValue("Constraint dropped")}});
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_SHOW_CONSTRAINTS) {
			const auto &show = dynamic_cast<const logical::LogicalShowConstraints &>(root);
			static_cast<void>(show);
			const std::vector<storage::LogicalGraphConstraintDefinition> definitions =
					backend_.listLogicalConstraints(maximumIndexes);
			QueryResult result;
			result.setColumns({"name", "type", "entity", "label", "properties"});
			for (const storage::LogicalGraphConstraintDefinition &definition: definitions) {
				context.checkGuard();
				result.addRow({{"name", PropertyValue(definition.name)},
						   {"type", PropertyValue(constraintKindName(definition.kind))},
						   {"entity", PropertyValue(constraintEntityKindName(definition.entity_kind))},
						   {"label", PropertyValue(definition.label)},
						   {"properties", PropertyValue(joinPropertyKeys(definition.property_keys))}});
			}
			return result;
		}
		throw std::runtime_error("logical schema runtime does not support this schema operator");
	}

} // namespace graph::query::execution
