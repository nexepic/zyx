/**
 * @file NativeConstraintDefinitionWriter.cpp
 * @brief Atomic native constraint catalog and support-tree staging.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/schema/NativeConstraintDefinitionWriter.hpp"

#include <utility>

#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	void NativeConstraintDefinitionWriter::stageCreate(
			NativeGraphWriteTransaction &transaction, SystemCatalog &catalog, SystemCatalogWriteSession &session,
			BTreePageStore &pages,
			RelocatableMetadataStore &metadata, ConstraintDefinition &definition,
			PageBackedGraphReadAdapter &graph,
			const NativeConstraintSupportTreeBuildOptions &options,
			const NativeConstraintPropertyTokenResolver &propertyTokenResolver) {
		if (!pages.usesMetadataStore(metadata) || !catalog.usesMetadataStore(metadata)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"canonical native constraint DDL requires one live metadata domain");
		}
		definition = session.createConstraintDefinition(std::move(definition));
		if (definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
				definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY) {
			BTreeInitialTreeMutationPlan supportTree = NativeConstraintSupportTreeBuilder::plan(
					pages, metadata, transaction.baseSnapshot(), definition, graph, options, propertyTokenResolver);
			transaction.stageScalarIndexCreate(pages, std::move(supportTree));
		}
		// The caller finishes the shared session after all DDL components are staged.
	}

	void NativeConstraintDefinitionWriter::stageDrop(NativeGraphWriteTransaction &transaction, SystemCatalog &catalog,
																					 SystemCatalogWriteSession &session, BTreePageStore &pages,
																 RelocatableMetadataStore &metadata,
																 const ConstraintDefinition &definition,
																 size_t maximumTreePages) {
		if (!pages.usesMetadataStore(metadata) || !catalog.usesMetadataStore(metadata)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"canonical native constraint DROP requires one live metadata domain");
		}
		const bool requiresSupportTree = definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
					definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY;
		const std::optional<IndexRootLocation> root =
				metadata.findIndexRootOptional(transaction.baseSnapshot(), definition.constraint_id);
		if (requiresSupportTree) {
			if (!root.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"canonical constraint DROP found no support-tree root");
			}
			transaction.stageScalarIndexDrop(pages, definition.constraint_id, maximumTreePages);
		} else if (root.has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"canonical constraint DROP found an unexpected support-tree root");
		}
		session.dropConstraintDefinition(definition);
	}

} // namespace graph::storage
