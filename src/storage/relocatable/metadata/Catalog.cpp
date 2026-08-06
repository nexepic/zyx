/**
 * @file Catalog.cpp
 * @brief Isolated relocatable segment catalog and physical-slot lifecycle implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/Catalog.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace graph::storage {

	namespace {

		enum class PhysicalSlotState : uint8_t {
			PSS_FREE = 0,
			PSS_RESERVED = 1,
			PSS_LIVE = 2,
			PSS_RETIRED = 3,
		};

		[[noreturn]] void throwCatalogError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		class PhysicalSlotAllocator {
		public:
			explicit PhysicalSlotAllocator(uint64_t maximumSlots) : maximum_slots_(maximumSlots) {}

			[[nodiscard]] PhysicalSlotId reserve() {
				if (!free_slots_.empty()) {
					const uint64_t index = free_slots_.back();
					free_slots_.pop_back();
					states_.at(static_cast<size_t>(index)) = PhysicalSlotState::PSS_RESERVED;
					return PhysicalSlotId(index + 1U);
				}
				if (states_.size() >= maximum_slots_) {
					throwCatalogError(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable physical slot ID overflow");
				}
				states_.push_back(PhysicalSlotState::PSS_RESERVED);
				return PhysicalSlotId(static_cast<uint64_t>(states_.size()));
			}

			// SegmentCatalog validates the higher-level lifecycle while holding its mutex.
			// Separate operations prevent this private allocator from accepting arbitrary states.
			void publishReserved(PhysicalSlotId slot) { setState(slot, PhysicalSlotState::PSS_LIVE); }

			void retireLive(PhysicalSlotId slot) { setState(slot, PhysicalSlotState::PSS_RETIRED); }

			void releaseReservation(PhysicalSlotId slot) {
				free_slots_.push_back(indexFor(slot));
				setState(slot, PhysicalSlotState::PSS_FREE);
			}

			void releaseRetired(PhysicalSlotId slot) {
				free_slots_.push_back(indexFor(slot));
				setState(slot, PhysicalSlotState::PSS_FREE);
			}

			[[nodiscard]] SegmentCatalogStats stats() const {
				SegmentCatalogStats result;
				result.total_slots = states_.size();
				result.free_slots = free_slots_.size();
				result.reserved_slots = std::count(states_.begin(), states_.end(), PhysicalSlotState::PSS_RESERVED);
				result.live_slots = std::count(states_.begin(), states_.end(), PhysicalSlotState::PSS_LIVE);
				result.retired_slots = std::count(states_.begin(), states_.end(), PhysicalSlotState::PSS_RETIRED);
				return result;
			}

		private:
			[[nodiscard]] uint64_t indexFor(PhysicalSlotId slot) const { return slot.value() - 1U; }

			void setState(PhysicalSlotId slot, PhysicalSlotState next) {
				const uint64_t index = indexFor(slot);
				states_.at(static_cast<size_t>(index)) = next;
			}

			uint64_t maximum_slots_;
			std::vector<PhysicalSlotState> states_;
			std::vector<uint64_t> free_slots_;
		};

		struct RetiredSlot {
			PhysicalSlotId slot{};
			SegmentId segment_id{};
			Generation generation{};
			LogSequenceNumber retired_lsn{};
		};

		struct PendingRelocation {
			RelocationPlan plan;
			bool copied = false;
		};

		using PinKey = std::pair<uint64_t, uint64_t>;

		[[nodiscard]] PinKey pinKey(SegmentId segmentId, Generation generation) {
			return {segmentId.value(), generation.value()};
		}

		[[nodiscard]] bool isKnownSegmentKind(SegmentKind kind) {
			return kind >= SegmentKind::SGK_ENTITY_PAGE_POOL &&
				   kind <= SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
		}

		void requireCurrentGeneration(const CatalogEntry &entry, Generation expectedGeneration) {
			if (entry.generation != expectedGeneration) {
				throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable catalog generation is stale");
			}
		}

		void requireNewerLsn(const CatalogEntry &entry, LogSequenceNumber nextLsn) {
			if (nextLsn.value() <= entry.committed_lsn.value()) {
				throwCatalogError(FormatError::FERR_INVALID_STATE,
								  "relocatable catalog LSN must advance a sealed generation");
			}
		}

		[[nodiscard]] Generation nextGeneration(Generation generation, uint64_t maximumGeneration) {
			if (generation.value() >= maximumGeneration) {
				throwCatalogError(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable generation overflow");
			}
			return Generation(generation.value() + 1U);
		}

		[[nodiscard]] bool hasAvailableCounter(uint64_t nextValue, uint64_t maximumValue) {
			return nextValue != 0 && nextValue <= maximumValue;
		}

		void advanceCounter(uint64_t &nextValue, uint64_t maximumValue) {
			nextValue = nextValue == maximumValue ? 0 : nextValue + 1U;
		}

	} // namespace

	struct SegmentCatalogState {
		mutable std::mutex mutex;
		SegmentCatalogLimits limits;
		uint64_t next_segment_id = 1;
		uint64_t next_relocation_id = 1;
		PhysicalSlotAllocator slots;
		std::unordered_map<uint64_t, CatalogEntry> entries;
		std::unordered_map<uint64_t, PendingRelocation> relocations;
		std::vector<RetiredSlot> retired_slots;
		std::map<PinKey, uint64_t> pins;
		std::map<uint64_t, uint64_t> snapshots;

		explicit SegmentCatalogState(SegmentCatalogLimits configuredLimits) :
			limits(configuredLimits), slots(limits.maximum_physical_slots) {}
	};

	namespace {

		[[nodiscard]] CatalogEntry &findEntry(SegmentCatalogState &state, SegmentId segmentId) {
			const auto it = state.entries.find(segmentId.value());
			if (it == state.entries.end()) {
				throwCatalogError(FormatError::FERR_INVALID_HEADER, "relocatable segment catalog entry does not exist");
			}
			return it->second;
		}

		[[nodiscard]] PendingRelocation &findRelocation(SegmentCatalogState &state, const RelocationPlan &plan) {
			const auto it = state.relocations.find(plan.relocation_id);
			if (it == state.relocations.end() || it->second.plan.segment_id != plan.segment_id ||
				it->second.plan.source_slot != plan.source_slot ||
				it->second.plan.destination_slot != plan.destination_slot ||
				it->second.plan.source_generation != plan.source_generation ||
				it->second.plan.destination_generation != plan.destination_generation) {
				throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable relocation plan is stale or invalid");
			}
			return it->second;
		}

		[[nodiscard]] bool hasPendingRelocation(const SegmentCatalogState &state, SegmentId segmentId) {
			return std::ranges::any_of(state.relocations, [&](const auto &candidate) {
				return candidate.second.plan.segment_id == segmentId;
			});
		}

		void releasePin(const std::shared_ptr<SegmentCatalogState> &state, const CatalogEntry &entry) {
			std::lock_guard<std::mutex> lock(state->mutex);
			const PinKey key = pinKey(entry.segment_id, entry.generation);
			auto it = state->pins.find(key);
			if (--it->second == 0) {
				state->pins.erase(it);
			}
		}

		void releaseSnapshot(const std::shared_ptr<SegmentCatalogState> &state, LogSequenceNumber lsn) {
			std::lock_guard<std::mutex> lock(state->mutex);
			auto it = state->snapshots.find(lsn.value());
			if (--it->second == 0) {
				state->snapshots.erase(it);
			}
		}

	} // namespace

	SegmentPin::SegmentPin(std::shared_ptr<SegmentCatalogState> state, CatalogEntry entry) :
		state_(std::move(state)), entry_(entry) {}

	SegmentPin::~SegmentPin() { release(); }

	SegmentPin::SegmentPin(SegmentPin &&other) noexcept : state_(std::move(other.state_)), entry_(other.entry_) {
		other.entry_ = {};
	}

	SegmentPin &SegmentPin::operator=(SegmentPin &&other) noexcept {
		if (this != &other) {
			release();
			state_ = std::move(other.state_);
			entry_ = other.entry_;
			other.entry_ = {};
		}
		return *this;
	}

	void SegmentPin::release() {
		if (state_) {
			releasePin(state_, entry_);
			state_.reset();
			entry_ = {};
		}
	}

	SnapshotPin::SnapshotPin(std::shared_ptr<SegmentCatalogState> state, LogSequenceNumber lsn) :
		state_(std::move(state)), lsn_(lsn) {}

	SnapshotPin::~SnapshotPin() { release(); }

	SnapshotPin::SnapshotPin(SnapshotPin &&other) noexcept : state_(std::move(other.state_)), lsn_(other.lsn_) {
		other.lsn_ = {};
	}

	SnapshotPin &SnapshotPin::operator=(SnapshotPin &&other) noexcept {
		if (this != &other) {
			release();
			state_ = std::move(other.state_);
			lsn_ = other.lsn_;
			other.lsn_ = {};
		}
		return *this;
	}

	void SnapshotPin::release() {
		if (state_) {
			releaseSnapshot(state_, lsn_);
			state_.reset();
			lsn_ = {};
		}
	}

	SegmentCatalog::SegmentCatalog(SegmentCatalogLimits limits) :
		state_(std::make_shared<SegmentCatalogState>(limits)) {}

	SegmentCatalog::~SegmentCatalog() = default;

	CatalogEntry SegmentCatalog::allocate(SegmentKind kind, uint64_t ownerId) {
		if (!isKnownSegmentKind(kind)) {
			throwCatalogError(FormatError::FERR_INVALID_ENUM, "relocatable catalog segment kind is invalid");
		}
		std::lock_guard<std::mutex> lock(state_->mutex);
		if (!hasAvailableCounter(state_->next_segment_id, state_->limits.maximum_segment_id)) {
			throwCatalogError(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable segment ID overflow");
		}
		if (state_->limits.maximum_generation == 0) {
			throwCatalogError(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable generation range is empty");
		}
		CatalogEntry entry;
		entry.segment_id = SegmentId(state_->next_segment_id);
		entry.physical_slot = state_->slots.reserve();
		entry.kind = kind;
		entry.owner_id = ownerId;
		entry.generation = Generation(1);
		entry.state = SegmentState::SGS_BUILDING;
		try {
			state_->entries.emplace(entry.segment_id.value(), entry);
		} catch (...) {
			state_->slots.releaseReservation(entry.physical_slot);
			throw;
		}
		advanceCounter(state_->next_segment_id, state_->limits.maximum_segment_id);
		return entry;
	}

	void SegmentCatalog::publish(SegmentId segmentId, Generation expectedGeneration, LogSequenceNumber committedLsn) {
		if (!committedLsn.isValid()) {
			throwCatalogError(FormatError::FERR_INVALID_HEADER, "relocatable published segment needs a committed LSN");
		}
		std::lock_guard<std::mutex> lock(state_->mutex);
		CatalogEntry &entry = findEntry(*state_, segmentId);
		requireCurrentGeneration(entry, expectedGeneration);
		if (entry.state != SegmentState::SGS_BUILDING) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable segment can only publish from building state");
		}
		state_->slots.publishReserved(entry.physical_slot);
		entry.state = SegmentState::SGS_SEALED;
		entry.committed_lsn = committedLsn;
	}

	void SegmentCatalog::retire(SegmentId segmentId, Generation expectedGeneration, LogSequenceNumber retiredLsn) {
		if (!retiredLsn.isValid()) {
			throwCatalogError(FormatError::FERR_INVALID_HEADER, "relocatable retired segment needs a committed LSN");
		}
		std::lock_guard<std::mutex> lock(state_->mutex);
		CatalogEntry &entry = findEntry(*state_, segmentId);
		requireCurrentGeneration(entry, expectedGeneration);
		if (entry.state != SegmentState::SGS_SEALED) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable segment can only retire from sealed state");
		}
		requireNewerLsn(entry, retiredLsn);
		if (hasPendingRelocation(*state_, segmentId)) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable segment cannot retire during relocation");
		}
		state_->slots.retireLive(entry.physical_slot);
		entry.state = SegmentState::SGS_RETIRED;
		entry.committed_lsn = retiredLsn;
		state_->retired_slots.push_back({entry.physical_slot, entry.segment_id, entry.generation, retiredLsn});
	}

	RelocationPlan SegmentCatalog::beginRelocation(SegmentId segmentId, Generation expectedGeneration) {
		std::lock_guard<std::mutex> lock(state_->mutex);
		CatalogEntry &entry = findEntry(*state_, segmentId);
		requireCurrentGeneration(entry, expectedGeneration);
		if (entry.state != SegmentState::SGS_SEALED) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable relocation source must be sealed");
		}
		if (hasPendingRelocation(*state_, segmentId)) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable segment already has a pending relocation");
		}
		if (!hasAvailableCounter(state_->next_relocation_id, state_->limits.maximum_relocation_id)) {
			throwCatalogError(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable relocation ID overflow");
		}
		RelocationPlan plan;
		plan.relocation_id = state_->next_relocation_id;
		plan.segment_id = segmentId;
		plan.source_slot = entry.physical_slot;
		plan.source_generation = entry.generation;
		plan.destination_generation = nextGeneration(entry.generation, state_->limits.maximum_generation);
		plan.destination_slot = state_->slots.reserve();
		try {
			state_->relocations.emplace(plan.relocation_id, PendingRelocation{plan});
		} catch (...) {
			state_->slots.releaseReservation(plan.destination_slot);
			throw;
		}
		advanceCounter(state_->next_relocation_id, state_->limits.maximum_relocation_id);
		return plan;
	}

	void SegmentCatalog::markRelocationCopied(const RelocationPlan &plan) {
		std::lock_guard<std::mutex> lock(state_->mutex);
		PendingRelocation &pending = findRelocation(*state_, plan);
		if (pending.copied) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable relocation payload was already copied");
		}
		pending.copied = true;
	}

	void SegmentCatalog::publishRelocation(const RelocationPlan &plan, LogSequenceNumber committedLsn) {
		if (!committedLsn.isValid()) {
			throwCatalogError(FormatError::FERR_INVALID_HEADER, "relocatable relocation needs a committed LSN");
		}
		std::lock_guard<std::mutex> lock(state_->mutex);
		const PendingRelocation &pending = findRelocation(*state_, plan);
		if (!pending.copied) {
			throwCatalogError(FormatError::FERR_INVALID_STATE,
							  "relocatable relocation cannot publish before payload validation");
		}
		CatalogEntry &entry = findEntry(*state_, plan.segment_id);
		requireCurrentGeneration(entry, plan.source_generation);
		requireNewerLsn(entry, committedLsn);
		state_->slots.publishReserved(plan.destination_slot);
		state_->slots.retireLive(plan.source_slot);
		state_->retired_slots.push_back({plan.source_slot, plan.segment_id, plan.source_generation, committedLsn});
		entry.physical_slot = plan.destination_slot;
		entry.generation = plan.destination_generation;
		entry.committed_lsn = committedLsn;
		state_->relocations.erase(plan.relocation_id);
	}

	void SegmentCatalog::abortRelocation(const RelocationPlan &plan) {
		std::lock_guard<std::mutex> lock(state_->mutex);
		static_cast<void>(findRelocation(*state_, plan));
		state_->slots.releaseReservation(plan.destination_slot);
		state_->relocations.erase(plan.relocation_id);
	}

	SegmentPin SegmentCatalog::pin(SegmentId segmentId) const {
		std::lock_guard<std::mutex> lock(state_->mutex);
		const CatalogEntry &entry = findEntry(*state_, segmentId);
		if (entry.state != SegmentState::SGS_SEALED) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable segment pin requires a sealed entry");
		}
		++state_->pins[pinKey(entry.segment_id, entry.generation)];
		return SegmentPin(state_, entry);
	}

	SnapshotPin SegmentCatalog::openSnapshot(LogSequenceNumber lsn) const {
		if (!lsn.isValid()) {
			throwCatalogError(FormatError::FERR_INVALID_HEADER, "relocatable snapshot needs a valid LSN");
		}
		std::lock_guard<std::mutex> lock(state_->mutex);
		++state_->snapshots[lsn.value()];
		return SnapshotPin(state_, lsn);
	}

	CatalogEntry SegmentCatalog::find(SegmentId segmentId) const {
		std::lock_guard<std::mutex> lock(state_->mutex);
		return findEntry(*state_, segmentId);
	}

	SegmentCatalogStats SegmentCatalog::stats() const {
		std::lock_guard<std::mutex> lock(state_->mutex);
		SegmentCatalogStats result = state_->slots.stats();
		result.catalog_entries = state_->entries.size();
		result.pending_relocations = state_->relocations.size();
		return result;
	}

	uint64_t SegmentCatalog::collectGarbage() {
		std::lock_guard<std::mutex> lock(state_->mutex);
		const uint64_t oldestSnapshot =
				state_->snapshots.empty() ? std::numeric_limits<uint64_t>::max() : state_->snapshots.begin()->first;
		uint64_t reclaimed = 0;
		auto retired = state_->retired_slots.begin();
		while (retired != state_->retired_slots.end()) {
			const bool pinned = state_->pins.contains(pinKey(retired->segment_id, retired->generation));
			if (pinned || oldestSnapshot <= retired->retired_lsn.value()) {
				++retired;
				continue;
			}
			state_->slots.releaseRetired(retired->slot);
			auto entry = state_->entries.find(retired->segment_id.value());
			if (entry->second.state == SegmentState::SGS_RETIRED) {
				state_->entries.erase(entry);
			}
			retired = state_->retired_slots.erase(retired);
			++reclaimed;
		}
		return reclaimed;
	}

	void SegmentCatalog::recover() {
		std::lock_guard<std::mutex> lock(state_->mutex);
		if (!state_->pins.empty() || !state_->snapshots.empty()) {
			throwCatalogError(FormatError::FERR_INVALID_STATE, "relocatable recovery requires no active pins or snapshots");
		}
		for (const auto &[relocationId, pending]: state_->relocations) {
			static_cast<void>(relocationId);
			state_->slots.releaseReservation(pending.plan.destination_slot);
		}
		state_->relocations.clear();
		for (auto it = state_->entries.begin(); it != state_->entries.end();) {
			if (it->second.state == SegmentState::SGS_BUILDING) {
				state_->slots.releaseReservation(it->second.physical_slot);
				it = state_->entries.erase(it);
			} else {
				++it;
			}
		}
		for (const RetiredSlot &retired: state_->retired_slots) {
			state_->slots.releaseRetired(retired.slot);
			auto entry = state_->entries.find(retired.segment_id.value());
			if (entry->second.state == SegmentState::SGS_RETIRED) {
				state_->entries.erase(entry);
			}
		}
		state_->retired_slots.clear();
	}

} // namespace graph::storage
