/**
 * @file ConstraintSupportTree.hpp
 * @brief Collision-safe bounded keys for internal relocatable constraint support trees.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "graph/core/PropertyTypes.hpp"

namespace graph::storage {

	// Support-tree keys are fixed width even when a constrained Property is a
	// large string, list, or map. The digest is only a routing prefix: callers
	// must compare the canonical Property bytes before accepting a collision.
	constexpr size_t RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES = 32U;
	constexpr size_t RSS_CONSTRAINT_SUPPORT_ENTITY_ID_BYTES = sizeof(uint64_t);
	constexpr size_t RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES =
			RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES + RSS_CONSTRAINT_SUPPORT_ENTITY_ID_BYTES;

	// The plan uses logical/batch identity only. Physical B+Tree changes remain
	// inside NativeGraphWriteTransaction and publish with the entity mutation.
	enum class ConstraintSupportTreeEntityReferenceKind : uint8_t {
		CSTREK_PERSISTED_NODE = 1,
		CSTREK_PERSISTED_EDGE = 2,
		CSTREK_CREATED_NODE = 3,
		CSTREK_CREATED_EDGE = 4,
	};

	struct ConstraintSupportTreeMutationPlan {
		uint64_t constraint_id = 0U;
		ConstraintSupportTreeEntityReferenceKind entity_reference_kind =
				ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE;
		uint64_t entity_reference_value = 0U;
		std::optional<std::vector<std::byte>> erased_canonical_key;
		std::optional<std::vector<std::byte>> upserted_canonical_key;
	};

	class ConstraintSupportTree final {
	public:
		[[nodiscard]] static std::vector<std::byte> canonicalKey(std::span<const PropertyValue> values);
		[[nodiscard]] static std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES>
		digest(std::span<const std::byte> canonicalKey);
		[[nodiscard]] static std::vector<std::byte> entryKey(std::span<const std::byte> canonicalKey,
													 uint64_t entityId);
		[[nodiscard]] static std::vector<std::byte>
		lowerBound(std::span<const std::byte> canonicalKey);
		[[nodiscard]] static bool matchesDigest(std::span<const std::byte> entryKey,
												 std::span<const std::byte> canonicalKey);
		[[nodiscard]] static uint64_t entryEntityId(std::span<const std::byte> entryKey);
	};

} // namespace graph::storage
