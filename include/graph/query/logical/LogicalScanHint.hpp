/**
 * @file LogicalScanHint.hpp
 * @brief Optional logical scan strategy hint.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

namespace graph::query::logical {

	// A logical hint never exposes a storage index or physical page layout.
	enum class LogicalScanHint {
		LSH_FULL,
		LSH_LABEL,
		LSH_PROPERTY,
		LSH_RANGE,
		LSH_COMPOSITE,
	};

} // namespace graph::query::logical
