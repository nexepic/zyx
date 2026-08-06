/**
 * @file StorageBackendFactory.cpp
 * @brief Fail-closed backend selection for existing database files.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/StorageBackendFactory.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/StorageFormat.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"

namespace graph::storage {

	StorageBackendSelection StorageBackendFactory::create(std::string database_path, size_t cache_size,
														  OpenMode open_mode) {
		const bool path_exists = std::filesystem::exists(database_path);
		if (open_mode == OpenMode::OPEN_CREATE_NEW_FILE && path_exists) {
			throw std::runtime_error("Database file already exists: " + database_path);
		}
		if (!path_exists &&
			(open_mode == OpenMode::OPEN_EXISTING_FILE || open_mode == OpenMode::OPEN_READ_ONLY_EXISTING_FILE)) {
			throw std::runtime_error("Database file does not exist: " + database_path);
		}
		if (!path_exists) {
			RelocatableStorageBackendOptions options;
			options.data_path = database_path;
			options.open_mode = open_mode;
			options.buffer_pool_bytes = cache_size;
			auto relocatable = RelocatableStorageBackend::initializeNew(std::move(options));
			return {relocatable, relocatable};
		}
		if (!std::filesystem::is_regular_file(database_path)) {
			throw std::runtime_error("Database path must name an existing regular file: " + database_path);
		}

		auto probe_file = std::make_shared<std::fstream>(database_path, std::ios::binary | std::ios::in);
		if (!probe_file->is_open()) { // ZYX_COV_EXCL_LINE: an existing regular file can disappear or lose permission
									  // only through an external filesystem race after validation.
			throw std::runtime_error("Cannot open database file for format inspection: " +
									 database_path); // ZYX_COV_EXCL_LINE: see the validation-race rationale above.
		}
		auto probe_io = std::make_shared<StorageIO>(probe_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		const StorageFormatInfo format = StorageFormatProbe::inspect(*probe_io);
		if (format.isRelocatable()) {
			static_cast<void>(SuperblockFile(probe_io).recover());
			RelocatableStorageBackendOptions options;
			options.data_path = database_path;
			options.buffer_pool_bytes = cache_size;
			// Relocatable preserves a caller's read-only request. Other existing-file
			// opens use its format-independent logical-write boundary.
			options.open_mode = open_mode == OpenMode::OPEN_READ_ONLY_EXISTING_FILE
										? OpenMode::OPEN_READ_ONLY_EXISTING_FILE
										: OpenMode::OPEN_EXISTING_FILE;
			auto relocatable = std::make_shared<RelocatableStorageBackend>(std::move(options));
			return {relocatable, relocatable};
		}
		throw std::runtime_error("Database refuses a " + std::string(StorageFormatProbe::describe(format.kind)) +
								 " file because it is not a complete native relocatable database");
	}

} // namespace graph::storage
