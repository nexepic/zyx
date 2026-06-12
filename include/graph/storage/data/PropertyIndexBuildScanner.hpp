/**
 * @file PropertyIndexBuildScanner.hpp
 * @brief Typed property-owner scanner for index build pipelines.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <span>
#include <string>
#include <vector>

#include "graph/core/Types.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::concurrent {
	class ThreadPool;
}

namespace graph::storage {

	class PropertyIndexBuildScanner {
	public:
		explicit PropertyIndexBuildScanner(const DataManager &dataManager);

		[[nodiscard]] std::vector<PropertyEntityOwnerScalarKeyValue> collect(
				EntityType ownerType,
				const std::vector<std::string> &keys,
				std::span<const int64_t> sortedOwnerIds = {},
				concurrent::ThreadPool *pool = nullptr) const;

	private:
		const DataManager &dataManager_;
	};

} // namespace graph::storage
