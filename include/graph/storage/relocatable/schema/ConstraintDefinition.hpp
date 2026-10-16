/**
 * @file ConstraintDefinition.hpp
 * @brief Versioned durable definition for one native relocatable graph constraint.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once
#include "graph/utils/OwnedStdVector.hpp"

#include "graph/utils/OwnedStdString.hpp"
#include "graph/utils/OwnedStringVector.hpp"
#include "graph/utils/OwnedStdVector.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_CONSTRAINT_DEFINITION_MAGIC = 0x4344595AU; // Little-endian bytes: ZYDC
	constexpr uint16_t RSS_CONSTRAINT_DEFINITION_VERSION = 1U;
	constexpr uint16_t RSS_CONSTRAINT_DEFINITION_HEADER_BYTES = 48U;
	constexpr uint32_t RSS_CONSTRAINT_DEFINITION_MAX_NAME_BYTES = 4096U;
	constexpr uint32_t RSS_CONSTRAINT_DEFINITION_MAX_PROPERTY_NAME_BYTES = 1024U;
	constexpr uint16_t RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES = 64U;

	// Prefix every member so Windows SDK macros cannot collide with schema values.
	enum class ConstraintDefinitionEntityKind : uint8_t {
		CDEK_NODE = 1,
		CDEK_EDGE = 2,
	};

	enum class ConstraintDefinitionKind : uint8_t {
		CDK_UNIQUE = 1,
		CDK_NOT_NULL = 2,
		CDK_PROPERTY_TYPE = 3,
		CDK_NODE_KEY = 4,
	};

	// This deliberately mirrors only Cypher's portable property-type constraints.
	// The value is encoded explicitly instead of persisting PropertyType's C++ enum.
	enum class ConstraintDefinitionPropertyType : uint8_t {
		CDPT_NONE = 0,
		CDPT_BOOLEAN = 1,
		CDPT_INTEGER = 2,
		CDPT_FLOAT = 3,
		CDPT_STRING = 4,
		CDPT_LIST = 5,
		CDPT_MAP = 6,
	};

	// Constraint IDs own an isolated high-bit B+Tree namespace. A UNIQUE or
	// NODE KEY definition additionally uses its ID as the support-tree ID; the
	// catalog is still separate from public index definitions and presentation.
	struct ConstraintDefinition {
        using OwnedHeapElement = void;
		uint64_t constraint_id = 0U;
		utils::OwnedStdString name;
		ConstraintDefinitionEntityKind entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
		ConstraintDefinitionKind kind = ConstraintDefinitionKind::CDK_UNIQUE;
		uint64_t schema_token_id = 0U;
		utils::OwnedStringVector property_names;
		ConstraintDefinitionPropertyType property_type = ConstraintDefinitionPropertyType::CDPT_NONE;

		bool operator==(const ConstraintDefinition &) const = default;
	};

	class ConstraintDefinitionCodec final {
	public:
        static void validate(const ConstraintDefinition &definition);
		[[nodiscard]] static std::vector<std::byte> encode(const ConstraintDefinition &definition);
        [[nodiscard]] static utils::OwnedStdVector<std::byte> encodeOwned(const ConstraintDefinition &definition);
		[[nodiscard]] static ConstraintDefinition decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
