/**
 * @file ConstraintMutationValidator.hpp
 * @brief Storage-format-independent validation for logical graph constraint mutations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"
#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"

namespace graph::storage {

	// This is the complete logical state required to evaluate a constraint. It
	// intentionally contains no physical page, segment, slot, or PropertyId.
	struct ConstraintValidationEntity {
		LogicalGraphMutationEntityKind kind = LogicalGraphMutationEntityKind::LGMK_NODE;
		uint64_t stable_id = 0U;
		std::vector<uint64_t> schema_tokens;
		std::unordered_map<std::string, PropertyValue> properties;
	};

	using ConstraintValidationEntityVisitor = std::function<void(const ConstraintValidationEntity &)>;

	// Every callback is bound to one MetadataSnapshotPin by its caller. Keeping
	// the pin outside this interface prevents a validator from accidentally
	// resolving names, entities, and support trees from different generations.
	struct ConstraintMutationValidationAdapter {
		std::function<ConstraintValidationEntity(LogicalGraphMutationEntityKind, uint64_t)> read_persisted_entity;
		std::function<void(LogicalGraphMutationEntityKind, size_t, const ConstraintValidationEntityVisitor &)>
				visit_persisted_entities;
		// Labels and relationship types are distinct token domains. Keeping the
		// callbacks separate prevents a same-named label and type from being
		// resolved through an ambiguous name-only lookup.
		std::function<uint64_t(std::string_view)> resolve_node_label_token;
		std::function<uint64_t(std::string_view)> resolve_relationship_type_token;
		std::function<bool(uint64_t)> has_support_tree;
		std::function<utils::OwnedStdVector<BTreePageEntry>(uint64_t, std::span<const std::byte>, size_t)> scan_support_tree;
	};

	struct ConstraintMutationValidatorOptions {
		size_t maximum_entity_scan = 0U;
		size_t maximum_support_collisions = 0U;
	};

	// Computes constraint violations and the support-tree transitions for one
	// declarative logical batch. It owns no storage and is equally usable by
	// fixed-record and native Entity-page transaction planners.
	class ConstraintMutationValidator final {
	public:
        // Values are borrowed in definition order; nullptr denotes absence.
        // Checks local non-null/type rules; uniqueness still needs support trees.
        static void validatePropertyValues(const ConstraintDefinition &definition,
            std::span<const PropertyValue *const> values);

		ConstraintMutationValidator(ConstraintMutationValidationAdapter adapter,
									ConstraintMutationValidatorOptions options);

		[[nodiscard]] std::vector<ConstraintSupportTreeMutationPlan>
		validate(std::span<const ConstraintDefinition> definitions, const LogicalGraphMutationBatch &batch,
				 std::span<const ConstraintDefinition> additionalDefinitions = {}) const;

	private:
		ConstraintMutationValidationAdapter adapter_;
		ConstraintMutationValidatorOptions options_;
	};

} // namespace graph::storage
