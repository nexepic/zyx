/**
 * @file LogicalProjectionSpecCodec.hpp
 * @brief Strict, storage-neutral decoder for gds.graph.project arguments.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/gds/LogicalGraphProjection.hpp"

namespace graph::query::gds {

	// Procedure maps arrive fully materialized from the Cypher literal evaluator.
	// These limits bound decoder CPU and the amount of copied configuration text;
	// graph materialization itself is bounded separately by LogicalProjectionBuildBounds.
	struct LogicalProjectionSpecCodecBounds {
		size_t maximum_arguments = 4U;
		size_t maximum_map_entries = 256U;
		size_t maximum_list_entries = 256U;
		size_t maximum_text_bytes = 4U * 1024U;
	};

	class LogicalProjectionSpecCodec final {
	public:
		[[nodiscard]] static LogicalGraphProjectionSpec decodeGraphProjectArguments(
				const std::vector<PropertyValue> &arguments,
				LogicalProjectionSpecCodecBounds bounds = {});

	private:
		LogicalProjectionSpecCodec() = delete;
	};

} // namespace graph::query::gds
