/**
 * @file StorageBackendFactory.hpp
 * @brief Selects an isolated physical backend from a validated file format.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "graph/storage/StorageBackend.hpp"
#include "graph/storage/StorageTypes.hpp"

namespace graph::storage {

	class RelocatableStorageBackend;

	// Retains the concrete handle only where callers need format-specific,
	// already-safe integration. The common backend remains the lifecycle owner.
	struct StorageBackendSelection {
		std::shared_ptr<StorageBackend> backend;
		std::shared_ptr<RelocatableStorageBackend> relocatable_storage;
	};

	class StorageBackendFactory final {
	public:
		// The normal Database opening path supports only the native relocatable
		// single-file format. All historical formats fail closed.
		[[nodiscard]] static StorageBackendSelection create(std::string database_path, size_t cache_size,
															OpenMode open_mode);
	};

} // namespace graph::storage
