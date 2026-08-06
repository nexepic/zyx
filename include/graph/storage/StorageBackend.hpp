/**
 * @file StorageBackend.hpp
 * @brief Common lifecycle and capability contract for physical storage backends.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include "graph/storage/StorageFormat.hpp"

namespace graph::storage {

	// This deliberately describes only lifecycle and externally supportable
	// operations. Query execution remains behind a separate logical-data adapter
	// so a new physical format can never be mistaken for legacy DataManager data.
	struct StorageBackendCapabilities {
		bool supports_logical_entity_reads = false;
		bool supports_logical_property_reads = false;
		bool supports_logical_state_reads = false;
		bool supports_query_execution = false;
		bool supports_read_write_transactions = false;

		constexpr bool operator==(const StorageBackendCapabilities &) const = default;
	};

	class StorageBackend {
	public:
		virtual ~StorageBackend() = default;

		virtual void open() = 0;
		virtual void close() = 0;
		virtual void flush() = 0;
		[[nodiscard]] virtual bool isOpen() const = 0;
		[[nodiscard]] virtual bool isReadOnly() const = 0;
		[[nodiscard]] virtual StorageFormatKind formatKind() const = 0;
		[[nodiscard]] virtual StorageBackendCapabilities capabilities() const = 0;
	};

} // namespace graph::storage
