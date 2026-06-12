/**
 * @file ColumnarBulkInput.hpp
 *
 * Shared columnar property input used by storage-native bulk ingest and
 * observer/index maintenance paths.
 */

#pragma once

#include <string>
#include <vector>

#include "graph/core/PropertyTypes.hpp"

namespace graph::storage {

	struct BulkPropertyColumn {
		std::string key;
		std::vector<PropertyValue> values;
	};

} // namespace graph::storage
