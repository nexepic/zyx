/**
 * @file MetadataSnapshotRegistry.hpp
 * @brief Runtime snapshot registration and writer serialization for relocatable metadata roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <shared_mutex>

#include "graph/storage/relocatable/format/SuperblockFile.hpp"

namespace graph::storage {

	class MetadataPagePoolAllocator;
	class RelocatableMetadataStore;
	struct MetadataSnapshotRegistryState;

	struct MetadataSnapshotRegistryLimits {
		uint64_t maximum_pins_per_generation = std::numeric_limits<uint64_t>::max();
	};

	class MetadataSnapshotPin final {
	public:
		MetadataSnapshotPin() = default;
		~MetadataSnapshotPin();
		MetadataSnapshotPin(const MetadataSnapshotPin &) = delete;
		MetadataSnapshotPin &operator=(const MetadataSnapshotPin &) = delete;
		MetadataSnapshotPin(MetadataSnapshotPin &&other) noexcept;
		MetadataSnapshotPin &operator=(MetadataSnapshotPin &&other) noexcept;

		[[nodiscard]] bool isValid() const { return state_ != nullptr; }
		[[nodiscard]] const Superblock &superblock() const { return superblock_; }
		void release();

	private:
		friend class MetadataSnapshotRegistry;
		MetadataSnapshotPin(std::shared_ptr<MetadataSnapshotRegistryState> state, Superblock superblock);

		std::shared_ptr<MetadataSnapshotRegistryState> state_;
		Superblock superblock_;
	};

	class MetadataWriteGuard final {
	public:
		MetadataWriteGuard() = default;
		MetadataWriteGuard(const MetadataWriteGuard &) = delete;
		MetadataWriteGuard &operator=(const MetadataWriteGuard &) = delete;
		MetadataWriteGuard(MetadataWriteGuard &&) = delete;
		MetadataWriteGuard &operator=(MetadataWriteGuard &&) = delete;

		[[nodiscard]] bool isValid() const { return lock_.owns_lock(); }
		void release();

	private:
		friend class MetadataSnapshotRegistry;
		MetadataWriteGuard(std::shared_ptr<MetadataSnapshotRegistryState> state,
						   std::unique_lock<std::shared_mutex> lock);

		std::shared_ptr<MetadataSnapshotRegistryState> state_;
		std::unique_lock<std::shared_mutex> lock_;
	};

	class MetadataSnapshotRegistry final {
	public:
		explicit MetadataSnapshotRegistry(SuperblockFile &bootstrap, MetadataSnapshotRegistryLimits limits = {});
		~MetadataSnapshotRegistry();
		MetadataSnapshotRegistry(const MetadataSnapshotRegistry &) = delete;
		MetadataSnapshotRegistry &operator=(const MetadataSnapshotRegistry &) = delete;

		// Reads and pins the newest durable root while holding the reader side of
		// the publication barrier. The returned root remains valid until release.
		[[nodiscard]] MetadataSnapshotPin openSnapshot() const;

		// Writers hold this thread-bound guard from recovering the current root
		// through publication and post-publication metadata reclamation.
		[[nodiscard]] MetadataWriteGuard beginWrite();
		// Creates a short-lived read pin for the current durable root while this
		// registry's writer guard is held. Typed preparers use it to validate
		// source bytes without attempting to recursively acquire the reader lock.
		[[nodiscard]] MetadataSnapshotPin pinCurrentUnderWrite(const MetadataWriteGuard &guard,
															   const Superblock &current) const;

		// Reclaims after publication while the supplied guard excludes new snapshot
		// registrations. `current` must be the newest durable Superblock.
		void reclaimAfterPublication(const MetadataWriteGuard &guard, MetadataPagePoolAllocator &allocator,
									 const Superblock &current) const;
		// Evaluates pinned durable roots while this registry's writer guard excludes
		// new registrations. Callers use this for generation-aware reclamation of a
		// physical object whose reachability is resolved through a COW directory.
		[[nodiscard]] bool anyActiveSnapshot(const MetadataWriteGuard &guard,
										 const std::function<bool(const Superblock &)> &predicate) const;

		[[nodiscard]] uint64_t activeSnapshotCount() const;

	private:
		friend class RelocatableMetadataStore;
		[[nodiscard]] bool owns(const MetadataSnapshotPin &snapshot) const;

		std::shared_ptr<MetadataSnapshotRegistryState> state_;
	};

} // namespace graph::storage
