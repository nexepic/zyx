/**
 * @file MetadataSnapshotRegistry.cpp
 * @brief Runtime lifetime tracking for relocatable copy-on-write metadata roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"

#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwSnapshotRegistryError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool sameSuperblock(const Superblock &left, const Superblock &right) { return left == right; }

		void releaseSnapshot(const std::shared_ptr<MetadataSnapshotRegistryState> &state, const Superblock &superblock);

	} // namespace

	struct MetadataSnapshotRegistryState {
		mutable std::shared_mutex publication_mutex;
		mutable std::mutex snapshots_mutex;
		SuperblockFile *bootstrap = nullptr;
		MetadataSnapshotRegistryLimits limits;
		std::map<uint64_t, std::pair<Superblock, uint64_t>> snapshots;

		MetadataSnapshotRegistryState(SuperblockFile &configuredBootstrap,
									  MetadataSnapshotRegistryLimits configuredLimits) :
			bootstrap(&configuredBootstrap), limits(configuredLimits) {}
	};

	namespace {

		void releaseSnapshot(const std::shared_ptr<MetadataSnapshotRegistryState> &state,
							 const Superblock &superblock) {
			std::lock_guard lock(state->snapshots_mutex);
			auto &snapshot = state->snapshots.at(superblock.generation.value());
			if (--snapshot.second == 0U) {
				state->snapshots.erase(superblock.generation.value());
			}
		}

		[[nodiscard]] std::vector<Superblock> activeSuperblocks(const MetadataSnapshotRegistryState &state) {
			std::lock_guard lock(state.snapshots_mutex);
			std::vector<Superblock> activeSnapshots;
			activeSnapshots.reserve(state.snapshots.size());
			for (const auto &[generation, snapshot]: state.snapshots) {
				static_cast<void>(generation);
				activeSnapshots.push_back(snapshot.first);
			}
			return activeSnapshots;
		}

	} // namespace

	MetadataSnapshotPin::MetadataSnapshotPin(std::shared_ptr<MetadataSnapshotRegistryState> state,
											 Superblock superblock) :
		state_(std::move(state)), superblock_(superblock) {}

	MetadataSnapshotPin::~MetadataSnapshotPin() { release(); }

	MetadataSnapshotPin::MetadataSnapshotPin(MetadataSnapshotPin &&other) noexcept :
		state_(std::move(other.state_)), superblock_(other.superblock_) {
		other.superblock_ = {};
	}

	MetadataSnapshotPin &MetadataSnapshotPin::operator=(MetadataSnapshotPin &&other) noexcept {
		if (this != &other) {
			release();
			state_ = std::move(other.state_);
			superblock_ = other.superblock_;
			other.superblock_ = {};
		}
		return *this;
	}

	void MetadataSnapshotPin::release() {
		if (state_) {
			releaseSnapshot(state_, superblock_);
			state_.reset();
			superblock_ = {};
		}
	}

	MetadataWriteGuard::MetadataWriteGuard(std::shared_ptr<MetadataSnapshotRegistryState> state,
										   std::unique_lock<std::shared_mutex> lock) :
		state_(std::move(state)), lock_(std::move(lock)) {}

	void MetadataWriteGuard::release() {
		if (lock_.owns_lock()) {
			lock_.unlock();
		}
		state_.reset();
	}

	MetadataSnapshotRegistry::MetadataSnapshotRegistry(SuperblockFile &bootstrap,
													   MetadataSnapshotRegistryLimits limits) :
		state_(std::make_shared<MetadataSnapshotRegistryState>(bootstrap, limits)) {
		if (limits.maximum_pins_per_generation == 0U) {
			throw std::invalid_argument("relocatable metadata snapshot registry needs a positive per-generation pin limit");
		}
	}

	MetadataSnapshotRegistry::~MetadataSnapshotRegistry() = default;

	MetadataSnapshotPin MetadataSnapshotRegistry::openSnapshot() const {
		std::shared_lock lock(state_->publication_mutex);
		const Superblock snapshot = state_->bootstrap->recover();
		static_cast<void>(SuperblockCodec::encode(snapshot));
		{
			std::lock_guard snapshotsLock(state_->snapshots_mutex);
			auto [position, inserted] =
					state_->snapshots.emplace(snapshot.generation.value(), std::make_pair(snapshot, 0U));
			static_cast<void>(inserted);
			if (position->second.second >= state_->limits.maximum_pins_per_generation) {
				throwSnapshotRegistryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										   "relocatable metadata snapshot reference count overflows");
			}
			++position->second.second;
		}
		return MetadataSnapshotPin(state_, snapshot);
	}

	MetadataWriteGuard MetadataSnapshotRegistry::beginWrite() {
		return MetadataWriteGuard(state_, std::unique_lock(state_->publication_mutex));
	}

	MetadataSnapshotPin MetadataSnapshotRegistry::pinCurrentUnderWrite(const MetadataWriteGuard &guard,
																	   const Superblock &current) const {
		if (!guard.isValid() || guard.state_.get() != state_.get() ||
			!sameSuperblock(state_->bootstrap->recover(), current)) {
			throwSnapshotRegistryError(
					FormatError::FERR_INVALID_STATE,
					"relocatable current snapshot pin requires this registry's active writer and durable root");
		}
		static_cast<void>(SuperblockCodec::encode(current));
		{
			std::lock_guard snapshotsLock(state_->snapshots_mutex);
			auto [position, inserted] =
					state_->snapshots.emplace(current.generation.value(), std::make_pair(current, 0U));
			static_cast<void>(inserted);
			if (position->second.second >= state_->limits.maximum_pins_per_generation) {
				throwSnapshotRegistryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										   "relocatable metadata snapshot reference count overflows");
			}
			++position->second.second;
		}
		return MetadataSnapshotPin(state_, current);
	}

	void MetadataSnapshotRegistry::reclaimAfterPublication(const MetadataWriteGuard &guard,
														   MetadataPagePoolAllocator &allocator,
														   const Superblock &current) const {
		if (!guard.isValid() || guard.state_.get() != state_.get()) {
			throwSnapshotRegistryError(FormatError::FERR_INVALID_STATE,
									   "relocatable metadata reclamation requires this registry's active writer guard");
		}
		if (!sameSuperblock(state_->bootstrap->recover(), current)) {
			throwSnapshotRegistryError(FormatError::FERR_INVALID_STATE,
									   "relocatable metadata reclamation requires the newest durable Superblock");
		}
		allocator.reclaimUnreachable(current, activeSuperblocks(*state_));
	}

	bool MetadataSnapshotRegistry::anyActiveSnapshot(
			const MetadataWriteGuard &guard, const std::function<bool(const Superblock &)> &predicate) const {
		if (!guard.isValid() || guard.state_.get() != state_.get() || !predicate) {
			throwSnapshotRegistryError(
					FormatError::FERR_INVALID_STATE,
					"relocatable active snapshot inspection requires this registry's active writer and predicate");
		}
		std::lock_guard lock(state_->snapshots_mutex);
		for (const auto &[generation, snapshot]: state_->snapshots) {
			static_cast<void>(generation);
			if (predicate(snapshot.first)) {
				return true;
			}
		}
		return false;
	}

	uint64_t MetadataSnapshotRegistry::activeSnapshotCount() const {
		std::lock_guard lock(state_->snapshots_mutex);
		uint64_t count = 0;
		for (const auto &[generation, snapshot]: state_->snapshots) {
			static_cast<void>(generation);
			count += snapshot.second;
		}
		return count;
	}

	bool MetadataSnapshotRegistry::owns(const MetadataSnapshotPin &snapshot) const {
		return snapshot.isValid() && snapshot.state_.get() == state_.get();
	}

} // namespace graph::storage
