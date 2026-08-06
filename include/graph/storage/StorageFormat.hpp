/**
 * @file StorageFormat.hpp
 * @brief Fail-closed boundary for the native relocatable database format.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>

namespace graph::storage {

	class StorageIO;

	// Prefixes keep the enum safe from Windows macros.
	enum class StorageFormatKind : uint8_t {
		SFM_EMPTY = 0,
		SFM_RELOCATABLE = 1,
		SFM_RELOCATABLE_INCOMPLETE = 2,
		SFM_TRUNCATED = 3,
		SFM_UNRECOGNIZED = 4,
	};

	struct StorageFormatInfo {
		StorageFormatKind kind = StorageFormatKind::SFM_UNRECOGNIZED;
		uint64_t file_size = 0;

		constexpr bool isRelocatable() const { return kind == StorageFormatKind::SFM_RELOCATABLE; }
		constexpr bool isEmpty() const { return kind == StorageFormatKind::SFM_EMPTY; }
	};

	// This boundary deliberately recognizes only complete format signatures. It
	// does not repair, reinterpret, or auto-convert a file selected for another
	// storage backend.
	class StorageFormatProbe final {
	public:
		[[nodiscard]] static StorageFormatInfo inspect(const StorageIO &io);
		[[nodiscard]] static const char *describe(StorageFormatKind kind);
	};

} // namespace graph::storage
