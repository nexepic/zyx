/**
 * @file Catalog.hpp
 * @brief Isolated relocatable segment catalog and physical-slot lifecycle primitives.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <limits>
#include <memory>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	struct SegmentCatalogState;

	struct CatalogEntry {
		SegmentId segment_id{};
		PhysicalSlotId physical_slot{};
		SegmentKind kind = SegmentKind::SGK_NONE;
		uint64_t owner_id = 0;
		Generation generation{};
		SegmentState state = SegmentState::SGS_FREE;
		LogSequenceNumber committed_lsn{};
	};

	struct RelocationPlan {
		uint64_t relocation_id = 0;
		SegmentId segment_id{};
		PhysicalSlotId source_slot{};
		PhysicalSlotId destination_slot{};
		Generation source_generation{};
		Generation destination_generation{};
	};

	struct SegmentCatalogStats {
		uint64_t total_slots = 0;
		uint64_t free_slots = 0;
		uint64_t reserved_slots = 0;
		uint64_t live_slots = 0;
		uint64_t retired_slots = 0;
		uint64_t catalog_entries = 0;
		uint64_t pending_relocations = 0;
	};

	// Explicit limits make file-capacity and counter-exhaustion paths testable.
	struct SegmentCatalogLimits {
		uint64_t maximum_physical_slots = std::numeric_limits<uint64_t>::max();
		uint64_t maximum_segment_id = std::numeric_limits<uint64_t>::max();
		uint64_t maximum_generation = std::numeric_limits<uint64_t>::max();
		uint64_t maximum_relocation_id = std::numeric_limits<uint64_t>::max();
	};

	class SegmentPin final {
	public:
		SegmentPin() = default;
		~SegmentPin();
		SegmentPin(const SegmentPin &) = delete;
		SegmentPin &operator=(const SegmentPin &) = delete;
		SegmentPin(SegmentPin &&other) noexcept;
		SegmentPin &operator=(SegmentPin &&other) noexcept;

		[[nodiscard]] bool isValid() const { return state_ != nullptr; }
		[[nodiscard]] const CatalogEntry &entry() const { return entry_; }
		void release();

	private:
		friend class SegmentCatalog;
		SegmentPin(std::shared_ptr<SegmentCatalogState> state, CatalogEntry entry);

		std::shared_ptr<SegmentCatalogState> state_;
		CatalogEntry entry_;
	};

	class SnapshotPin final {
	public:
		SnapshotPin() = default;
		~SnapshotPin();
		SnapshotPin(const SnapshotPin &) = delete;
		SnapshotPin &operator=(const SnapshotPin &) = delete;
		SnapshotPin(SnapshotPin &&other) noexcept;
		SnapshotPin &operator=(SnapshotPin &&other) noexcept;

		[[nodiscard]] bool isValid() const { return state_ != nullptr; }
		[[nodiscard]] LogSequenceNumber lsn() const { return lsn_; }
		void release();

	private:
		friend class SegmentCatalog;
		SnapshotPin(std::shared_ptr<SegmentCatalogState> state, LogSequenceNumber lsn);

		std::shared_ptr<SegmentCatalogState> state_;
		LogSequenceNumber lsn_{};
	};

	class SegmentCatalog final {
	public:
		explicit SegmentCatalog(SegmentCatalogLimits limits = {});
		~SegmentCatalog();
		SegmentCatalog(const SegmentCatalog &) = delete;
		SegmentCatalog &operator=(const SegmentCatalog &) = delete;

		[[nodiscard]] CatalogEntry allocate(SegmentKind kind, uint64_t ownerId);
		void publish(SegmentId segmentId, Generation expectedGeneration, LogSequenceNumber committedLsn);
		void retire(SegmentId segmentId, Generation expectedGeneration, LogSequenceNumber retiredLsn);

		[[nodiscard]] RelocationPlan beginRelocation(SegmentId segmentId, Generation expectedGeneration);
		void markRelocationCopied(const RelocationPlan &plan);
		void publishRelocation(const RelocationPlan &plan, LogSequenceNumber committedLsn);
		void abortRelocation(const RelocationPlan &plan);

		[[nodiscard]] SegmentPin pin(SegmentId segmentId) const;
		[[nodiscard]] SnapshotPin openSnapshot(LogSequenceNumber lsn) const;
		[[nodiscard]] CatalogEntry find(SegmentId segmentId) const;
		[[nodiscard]] SegmentCatalogStats stats() const;

		// Reclaim only generations that no active snapshot or pin can still observe.
		[[nodiscard]] uint64_t collectGarbage();
		// Model an open-time recovery pass: discard unpublished work and orphaned retired bytes.
		void recover();

	private:
		std::shared_ptr<SegmentCatalogState> state_;
	};

} // namespace graph::storage
