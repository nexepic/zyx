/**
 * @file RelocatableIntegrityScanner.hpp
 * @brief Read-only cross-directory integrity verification for relocatable storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <memory>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {
	class StorageIO;
}

namespace graph::storage {

	class RelocatableMetadataStore;

	struct RelocatableIntegrityReport {
		Generation generation{};
		LogSequenceNumber checkpoint_lsn{};
		uint64_t catalog_entries = 0;
		uint64_t sealed_segments = 0;
		uint64_t queued_retired_slots = 0;
		uint64_t page_locations = 0;
		uint64_t extent_locations = 0;
		uint64_t state_locations = 0;
		uint64_t vector_locations = 0;
		uint64_t vector_adjacencies = 0;
		uint64_t vector_manifests = 0;
		uint64_t large_value_manifest_routes = 0;
		uint64_t large_value_manifest_nodes = 0;
		uint64_t large_value_data_pages = 0;
		uint64_t large_value_content_index_entries = 0;

		constexpr bool operator==(const RelocatableIntegrityReport &) const = default;
	};

	// Scans one immutable metadata snapshot and never reconciles, allocates, or
	// repairs storage. Any malformed cross-reference fails closed with a
	// FormatException before the report is returned.
	class RelocatableIntegrityScanner final {
	public:
		RelocatableIntegrityScanner(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata);

		[[nodiscard]] RelocatableIntegrityReport scan() const;

	private:
		std::shared_ptr<StorageIO> io_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
