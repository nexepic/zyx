/**
 * @file LogicalPlanSchemaRuntime.cpp
 * @brief Implementation of the fail-closed logical DDL runtime.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/LogicalPlanSchemaRuntime.hpp"

#include <cstdint>
#include <array>
#include <initializer_list>
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
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

		[[nodiscard]] std::string_view indexKindName(storage::LogicalGraphIndexKind kind) {
			switch (kind) {
				case storage::LogicalGraphIndexKind::LGIK_PROPERTY: return "property";
				case storage::LogicalGraphIndexKind::LGIK_COMPOSITE: return "composite";
				case storage::LogicalGraphIndexKind::LGIK_VECTOR: return "vector";
			}
			throw std::runtime_error("logical schema runtime received an unknown Index kind");
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

		[[nodiscard]] std::string_view constraintKindName(storage::LogicalGraphConstraintKind kind) {
			switch (kind) {
				case storage::LogicalGraphConstraintKind::LGCK_UNIQUE: return "unique";
				case storage::LogicalGraphConstraintKind::LGCK_NOT_NULL: return "not_null";
				case storage::LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE: return "property_type";
				case storage::LogicalGraphConstraintKind::LGCK_NODE_KEY: return "node_key";
			}
			throw std::runtime_error("logical schema runtime received an unknown constraint kind");
		}

		[[nodiscard]] std::string_view constraintEntityKindName(storage::LogicalGraphConstraintEntityKind kind) {
			switch (kind) {
				case storage::LogicalGraphConstraintEntityKind::LGCEK_NODE: return "node";
				case storage::LogicalGraphConstraintEntityKind::LGCEK_EDGE: return "edge";
			}
			throw std::runtime_error("logical schema runtime received an unknown constraint entity kind");
		}

        [[nodiscard]] size_t requestStringCopyUpper(std::initializer_list<std::string_view> strings) {
            size_t bytes = 0U;
            for (const auto text : strings) bytes = utils::detail::addStandardHeapBytes(bytes,
                utils::detail::standardFreshStringHeapUpper(text.size()));
            return bytes;
        }

	} // namespace

	QueryResult LogicalPlanSchemaRuntime::execute(const logical::LogicalOperator &root, size_t maximumIndexes,
																			 const QueryContext &context, size_t maximumPropertySourceBytes) const {
		if (maximumIndexes == 0U) {
			throw std::invalid_argument("logical schema runtime requires a positive Index result bound");
		}
		context.checkGuardNow();
        std::shared_ptr<std::pmr::memory_resource> memory = utils::OwnedAllocationScope::owner();
        if (context.resource_account) memory = std::make_shared<MaterializedByteBudget>(
            maximumPropertySourceBytes != 0U ? maximumPropertySourceBytes : context.resource_account->maximum(),
            context.resource_account, std::pmr::new_delete_resource());
        utils::OwnedAllocationScope allocation(memory);
        const auto emptyResult = [&] {
            return context.resource_account ? QueryResult(detail::ResourceLease(context.resource_account)) : QueryResult{};
        };
        const auto confirmation = [&](std::string_view message) {
            auto result = emptyResult();
            const std::array<std::string_view, 1U> columns{"result"};
            const std::array<StringResultField, 1U> fields{{{"result", message}}};
            result.setColumnViews(columns); result.addStringRow(fields);
            return result;
        };

		std::function<void()> checkpoint;
		if (context.guard) checkpoint = [&context] { context.checkGuardNow(); };
		std::function<void(size_t)> propertyAdmission;
		if (maximumPropertySourceBytes != 0U || context.guard) {
			propertyAdmission = [&context, maximumPropertySourceBytes](size_t bytes) {
				context.checkGuardNow();
				if (maximumPropertySourceBytes != 0U && bytes > maximumPropertySourceBytes)
					throw std::runtime_error("logical schema Property source batch exceeded its byte bound");
			};
		}
		if (root.getType() == logical::LogicalOpType::LOP_CREATE_INDEX) {
			const auto &create = dynamic_cast<const logical::LogicalCreateIndex &>(root);
			if (create.getLabel().empty() || create.getPropertyKeys().empty()) {
				throw std::logic_error("logical CREATE INDEX has an incomplete node Property schema");
			}
            auto result = confirmation("Index created");
            utils::OwnedHeapReservation requestMemory(memory, utils::detail::addStandardHeapBytes(
                requestStringCopyUpper({create.getIndexName(), create.getLabel()}),
                graph::query::detail::resultStringsCopyHeapUpper(create.getPropertyKeys())));
			const storage::LogicalGraphIndexDefinition definition = backend_.createLogicalNodeIndexWithCheckpoint(
					{create.getIndexName(), create.getLabel(), create.getPropertyKeys()},
					checkpoint, propertyAdmission);
			if (definition.index_id == 0U || definition.name.empty()) {
				throw std::logic_error("logical CREATE INDEX backend returned an invalid durable definition");
			}
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
            auto result = confirmation("Index created");
            utils::OwnedHeapReservation requestMemory(memory, requestStringCopyUpper({
                create.getIndexName(), create.getLabel(), create.getProperty(), create.getMetric()}));
			const storage::LogicalGraphIndexDefinition definition = backend_.createLogicalNodeVectorIndexWithCheckpoint(
					{create.getIndexName(), create.getLabel(), create.getProperty(),
					 static_cast<uint32_t>(create.getDimension()), create.getMetric()},
					checkpoint, propertyAdmission);
			if (definition.index_id == 0U || definition.name.empty() ||
				definition.kind != storage::LogicalGraphIndexKind::LGIK_VECTOR) {
				throw std::logic_error("logical CREATE VECTOR INDEX backend returned an invalid durable definition");
			}
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_DROP_INDEX) {
			const auto &drop = dynamic_cast<const logical::LogicalDropIndex &>(root);
            auto result = confirmation("Index dropped");
			const bool dropped = !drop.getIndexName().empty()
									 ? backend_.dropLogicalIndexByName(drop.getIndexName())
									 : backend_.dropLogicalNodeIndexBySchema(drop.getLabel(), drop.getPropertyKey());
			if (!dropped) {
				return emptyResult();
			}
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_SHOW_INDEXES) {
            const auto definitions = backend_.listOwnedLogicalIndexes(maximumIndexes);
            auto result = emptyResult();
            const std::array<std::string_view, 4U> columns{"name", "type", "label", "properties"};
            result.setColumnViews(columns);
            for (const auto &definition : definitions) {
                context.checkGuard();
                const std::array fields{StringResultField{"name", definition.name},
                    StringResultField{"type", indexKindName(definition.kind)},
                    StringResultField{"label", definition.label},
                    StringResultField::joined("properties", std::span<const std::string>(definition.property_keys.data(), definition.property_keys.size()), ",")};
                result.addStringRow(fields);
            }
            return result;
        }

		if (root.getType() == logical::LogicalOpType::LOP_CREATE_CONSTRAINT) {
			const auto &create = dynamic_cast<const logical::LogicalCreateConstraint &>(root);
			if (create.getName().empty() || create.getLabel().empty() || create.getProperties().empty()) {
				throw std::logic_error("logical CREATE CONSTRAINT has an incomplete schema");
			}
            auto result = confirmation("Constraint created");
            utils::OwnedHeapReservation requestMemory(memory, utils::detail::addStandardHeapBytes(
                requestStringCopyUpper({create.getName(), create.getLabel()}),
                graph::query::detail::resultStringsCopyHeapUpper(create.getProperties())));
			const storage::LogicalGraphConstraintDefinition definition = backend_.createLogicalConstraintWithCheckpoint(
					{create.getName(), constraintEntityKind(create.getEntityType()),
					 constraintKind(create.getConstraintType()), create.getLabel(), create.getProperties(),
					 constraintPropertyType(create.getOptions())},
					checkpoint, propertyAdmission);
			if (definition.constraint_id == 0U || definition.name.empty()) {
				throw std::logic_error("logical CREATE CONSTRAINT backend returned an invalid durable definition");
			}
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_DROP_CONSTRAINT) {
			const auto &drop = dynamic_cast<const logical::LogicalDropConstraint &>(root);
            auto result = confirmation("Constraint dropped");
			const bool dropped = backend_.dropLogicalConstraintByName(drop.getName());
			if (!dropped) {
				if (!drop.isIfExists()) {
					throw std::runtime_error("Constraint '" + drop.getName() + "' does not exist");
				}
				return emptyResult();
			}
			return result;
		}
		if (root.getType() == logical::LogicalOpType::LOP_SHOW_CONSTRAINTS) {
            const auto definitions = backend_.listOwnedLogicalConstraints(maximumIndexes);
            auto result = emptyResult();
            const std::array<std::string_view, 5U> columns{"name", "type", "entity", "label", "properties"};
            result.setColumnViews(columns);
            for (const auto &definition : definitions) {
                context.checkGuard();
                const std::array fields{StringResultField{"name", definition.name},
                    StringResultField{"type", constraintKindName(definition.kind)},
                    StringResultField{"entity", constraintEntityKindName(definition.entity_kind)},
                    StringResultField{"label", definition.label},
                    StringResultField::joined("properties", std::span<const std::string>(definition.property_keys.data(), definition.property_keys.size()), ",")};
                result.addStringRow(fields);
            }
            return result;
        }

		throw std::runtime_error("logical schema runtime does not support this schema operator");
	}

} // namespace graph::query::execution
