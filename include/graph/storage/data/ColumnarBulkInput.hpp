/**
 * @file ColumnarBulkInput.hpp
 *
 * Shared columnar property input used by storage-native bulk ingest and
 * observer/index maintenance paths.
 */

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"

namespace graph::storage {

	struct BulkPropertyColumn {
		std::string key;
		std::vector<PropertyValue> values;
	};

	class BulkInputError final : public std::invalid_argument {
	public:
		BulkInputError(size_t rowIndex, std::string fieldPath, std::string message) :
				std::invalid_argument(std::move(message)), rowIndex_(rowIndex), fieldPath_(std::move(fieldPath)) {}

		[[nodiscard]] size_t rowIndex() const noexcept { return rowIndex_; }
		[[nodiscard]] const std::string &fieldPath() const noexcept { return fieldPath_; }

	private:
		size_t rowIndex_;
		std::string fieldPath_;
	};

} // namespace graph::storage
