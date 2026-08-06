/**
 * @file MetadataRetirement.cpp
 * @brief Snapshot-safe validation for relocatable physical-slot retirement.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"

#include <cstdint>
#include <optional>

#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	MetadataRetirement::MetadataRetirement(CatalogTree &catalog, LocationDirectory &locations,
												 MetadataSnapshotRegistry &snapshots) :
		catalog_(catalog), locations_(locations), snapshots_(snapshots) {}

	void MetadataRetirement::prepareQueueInsertions(MetadataWriteBatch &batch,
															std::span<const RetireQueueEntry> entries) const {
		if (entries.empty()) {
			return;
		}
		uint64_t previousSlot = 0U;
		for (const RetireQueueEntry &entry: entries) {
			const uint8_t malformed =
					static_cast<uint8_t>(!entry.physical_slot.isValid()) |
					static_cast<uint8_t>(entry.physical_slot.value() <= previousSlot) |
					static_cast<uint8_t>(!entry.segment_id.isValid()) |
					static_cast<uint8_t>(entry.retired_generation != batch.generation()) |
					static_cast<uint8_t>(entry.disposition != RetireQueueDisposition::RQD_RELOCATED_SOURCE &&
														 entry.disposition != RetireQueueDisposition::RQD_RETIRED_SEGMENT);
			if (malformed != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Retire Queue insertion is malformed or duplicates a durable slot");
			}
			if (locations_.findRetireQueueEntryOptional(batch.current(), entry.physical_slot).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Retire Queue insertion is malformed or duplicates a durable slot");
			}
			previousSlot = entry.physical_slot.value();
		}
		locations_.prepareReplaceRetireQueue(batch, entries, {});
	}

	bool MetadataRetirement::isVisibleToActiveSnapshot(const MetadataWriteGuard &writer,
															 const RetireQueueEntry &entry) const {
		return snapshots_.anyActiveSnapshot(writer, [this, &entry](const Superblock &snapshot) {
			// A snapshot at or before retirement necessarily selected roots that
			// could resolve the old placement. For later snapshots, inspect the
			// immutable Catalog mapping as a defensive check against a stale
			// retirement transition that remains reachable through a newer root.
			if (snapshot.generation <= entry.retired_generation) {
				return true;
			}
			const std::optional<CatalogEntry> visible = catalog_.findOptional(snapshot, entry.segment_id);
			return visible.has_value() && visible->physical_slot == entry.physical_slot;
		});
	}

} // namespace graph::storage
