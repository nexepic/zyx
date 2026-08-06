/**
 * @file LogicalGraphSchema.hpp
 * @brief Format-independent logical schema index boundary.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace graph::storage {

	// Prefixes make these public enum values safe when Windows headers define
	// short macro names such as CREATE or DROP.
	enum class LogicalGraphIndexKind : uint8_t {
		LGIK_PROPERTY = 1,
		LGIK_COMPOSITE = 2,
		LGIK_VECTOR = 3,
	};

	struct LogicalGraphIndexDefinition {
		uint64_t index_id = 0U;
		std::string name;
		LogicalGraphIndexKind kind = LogicalGraphIndexKind::LGIK_PROPERTY;
		std::string label;
		std::vector<std::string> property_keys;
		uint32_t vector_dimensions = 0U;
		std::string vector_metric;

		bool operator==(const LogicalGraphIndexDefinition &) const = default;
	};

	struct LogicalGraphNodeIndexCreateRequest {
		std::string name;
		std::string label;
		std::vector<std::string> property_keys;

		bool operator==(const LogicalGraphNodeIndexCreateRequest &) const = default;
	};

	// Vector indexes have a deliberately separate request shape: exactly one
	// Property, a positive fixed dimension, and a normalized metric are part of
	// the durable semantic identity rather than optional scalar-index metadata.
	struct LogicalGraphNodeVectorIndexCreateRequest {
		std::string name;
		std::string label;
		std::string property_key;
		uint32_t dimensions = 0U;
		std::string metric;

		bool operator==(const LogicalGraphNodeVectorIndexCreateRequest &) const = default;
	};

	// Constraint schema is deliberately independent from index schema. A
	// constraint may own an internal support tree, but its identity, lifecycle,
	// and presentation must never be inferred from a public index definition.
	enum class LogicalGraphConstraintEntityKind : uint8_t {
		LGCEK_NODE = 1,
		LGCEK_EDGE = 2,
	};

	enum class LogicalGraphConstraintKind : uint8_t {
		LGCK_UNIQUE = 1,
		LGCK_NOT_NULL = 2,
		LGCK_PROPERTY_TYPE = 3,
		LGCK_NODE_KEY = 4,
	};

	enum class LogicalGraphConstraintPropertyType : uint8_t {
		LGCPT_NONE = 0,
		LGCPT_BOOLEAN = 1,
		LGCPT_INTEGER = 2,
		LGCPT_FLOAT = 3,
		LGCPT_STRING = 4,
		LGCPT_LIST = 5,
		LGCPT_MAP = 6,
	};

	struct LogicalGraphConstraintDefinition {
		uint64_t constraint_id = 0U;
		std::string name;
		LogicalGraphConstraintEntityKind entity_kind = LogicalGraphConstraintEntityKind::LGCEK_NODE;
		LogicalGraphConstraintKind kind = LogicalGraphConstraintKind::LGCK_UNIQUE;
		std::string label;
		std::vector<std::string> property_keys;
		LogicalGraphConstraintPropertyType property_type = LogicalGraphConstraintPropertyType::LGCPT_NONE;

		bool operator==(const LogicalGraphConstraintDefinition &) const = default;
	};

	struct LogicalGraphConstraintCreateRequest {
		std::string name;
		LogicalGraphConstraintEntityKind entity_kind = LogicalGraphConstraintEntityKind::LGCEK_NODE;
		LogicalGraphConstraintKind kind = LogicalGraphConstraintKind::LGCK_UNIQUE;
		std::string label;
		std::vector<std::string> property_keys;
		LogicalGraphConstraintPropertyType property_type = LogicalGraphConstraintPropertyType::LGCPT_NONE;

		bool operator==(const LogicalGraphConstraintCreateRequest &) const = default;
	};

	// A schema implementation owns complete initial index construction and only
	// returns after the durable definition is query-visible. It is kept separate
	// from logical data-write transactions: schema DDL is auto-commit and must
	// never become partially visible through a user transaction overlay.
	class LogicalGraphSchemaBackend {
	public:
		virtual ~LogicalGraphSchemaBackend() = default;

		[[nodiscard]] virtual LogicalGraphIndexDefinition
		createLogicalNodeIndex(const LogicalGraphNodeIndexCreateRequest &request) = 0;
		// A batch becomes query-visible only after every definition has been built
		// and committed. This preserves public all-or-nothing schema semantics.
		[[nodiscard]] virtual std::vector<LogicalGraphIndexDefinition>
		createLogicalNodeIndexes(std::span<const LogicalGraphNodeIndexCreateRequest> requests) = 0;
		[[nodiscard]] virtual LogicalGraphIndexDefinition
		createLogicalNodeVectorIndex(const LogicalGraphNodeVectorIndexCreateRequest &request) = 0;
		[[nodiscard]] virtual bool dropLogicalIndexByName(std::string_view name) = 0;
		[[nodiscard]] virtual bool dropLogicalNodeIndexBySchema(std::string_view label,
																		 std::string_view propertyKey) = 0;
		[[nodiscard]] virtual std::vector<LogicalGraphIndexDefinition>
		listLogicalIndexes(size_t maximumIndexes) const = 0;
		[[nodiscard]] virtual LogicalGraphConstraintDefinition
		createLogicalConstraint(const LogicalGraphConstraintCreateRequest &request) = 0;
		[[nodiscard]] virtual bool dropLogicalConstraintByName(std::string_view name) = 0;
		[[nodiscard]] virtual std::vector<LogicalGraphConstraintDefinition>
		listLogicalConstraints(size_t maximumConstraints) const = 0;
	};

} // namespace graph::storage
