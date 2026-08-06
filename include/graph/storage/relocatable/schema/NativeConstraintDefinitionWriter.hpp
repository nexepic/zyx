/**
 * @file NativeConstraintDefinitionWriter.hpp
 * @brief Atomic native constraint catalog and support-tree staging.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include "graph/storage/relocatable/schema/NativeConstraintSupportTreeBuilder.hpp"

namespace graph::storage {

	class NativeGraphWriteTransaction;
	class PageBackedGraphReadAdapter;
	class SystemCatalog;
	class SystemCatalogWriteSession;

	// DDL callers use this boundary instead of staging a support tree and its
	// catalog directions independently. The plan remains unpublished until the
	// enclosing native graph transaction commits.
	class NativeConstraintDefinitionWriter final {
	public:
		static void stageCreate(NativeGraphWriteTransaction &transaction, SystemCatalog &catalog,
								SystemCatalogWriteSession &catalogSession,
								BTreePageStore &pages, RelocatableMetadataStore &metadata,
								ConstraintDefinition &definition, PageBackedGraphReadAdapter &graph,
								const NativeConstraintSupportTreeBuildOptions &options,
								const NativeConstraintPropertyTokenResolver &propertyTokenResolver);
		static void stageDrop(NativeGraphWriteTransaction &transaction, SystemCatalog &catalog,
							  SystemCatalogWriteSession &catalogSession,
							  BTreePageStore &pages, RelocatableMetadataStore &metadata,
							  const ConstraintDefinition &definition, size_t maximumTreePages);
	};

} // namespace graph::storage
