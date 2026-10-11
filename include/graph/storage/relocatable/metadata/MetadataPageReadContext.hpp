/**
 * @file MetadataPageReadContext.hpp
 * @brief Bounded decoded metadata reuse owned by a live snapshot pin.
 */
#pragma once

#include <deque>
#include <limits>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <optional>
#include <typeindex>
#include <unordered_map>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/Superblock.hpp"

namespace graph::storage {

	// Only immutable, unresolved codec results belong here. Logical child
	// resolution and caller-specific owner/visibility checks remain outside.
	class MetadataPageReadContext final {
	public:
		static constexpr size_t MAXIMUM_ENTRIES = 128U;
		static constexpr size_t MAXIMUM_CHARGED_BYTES = 1024U * 1024U;

		explicit MetadataPageReadContext(Superblock snapshot,
				size_t maximumBytes = MAXIMUM_CHARGED_BYTES) : snapshot_(snapshot), maximum_bytes_(maximumBytes) {
			static_cast<void>(SuperblockCodec::encode(snapshot_));
			if (maximumBytes > MAXIMUM_CHARGED_BYTES) throw std::invalid_argument("metadata cache exceeds its byte limit");
		}

		class Scope final {
		public:
			Scope(std::shared_ptr<MetadataPageReadContext> context, const StorageIO &io) :
				context_(std::move(context)), previous_(active_) {
				if (!context_) throw std::invalid_argument("metadata read scope requires a live context");
				std::lock_guard lock(context_->mutex_);
				if (context_->io_ != nullptr && context_->io_ != &io) {
					throw std::invalid_argument("metadata read context cannot cross StorageIO identities");
				}
				if (context_->io_ == nullptr) context_->io_ = &io;
				context_->synchronizeWrites();
				active_ = context_.get();
			}
			~Scope() { active_ = previous_; }
			Scope(const Scope &) = delete;
			Scope &operator=(const Scope &) = delete;
		private:
			std::shared_ptr<MetadataPageReadContext> context_;
			MetadataPageReadContext *previous_;
		};

		[[nodiscard]] static MetadataPageReadContext *current(const StorageIO &io) {
			return active_ != nullptr && active_->io_ == &io ? active_ : nullptr;
		}
		[[nodiscard]] static MetadataPageReadContext *current(const StorageIO &io, const Superblock &view) {
			return active_ != nullptr && active_->io_ == &io &&
				active_->snapshot_.database_id == view.database_id &&
				active_->snapshot_.data_region_offset == view.data_region_offset ? active_ : nullptr;
		}
		[[nodiscard]] bool authenticates(const Superblock &context) const { return context == snapshot_; }

		template<typename Page>
		[[nodiscard]] std::optional<Page> find(PhysicalPageRef reference) {
			std::lock_guard lock(mutex_);
			synchronizeWrites();
			const auto found = entries_.find(key<Page>(reference));
			if (found == entries_.end()) return std::nullopt;
			debug::PerfTrace::addValue("native_graph.read.decoded_metadata_cache_hits", 1);
			return *std::static_pointer_cast<const Page>(found->second.page);
		}

		template<typename Page>
		void put(PhysicalPageRef reference, const Page &page, size_t variableBytes = 0U) {
			std::lock_guard lock(mutex_);
			synchronizeWrites();
			// Future/private COW images are never admitted to the published context.
			if (reference.generation > snapshot_.generation ||
				io_->dataMutationEpoch() == std::numeric_limits<uint64_t>::max()) return;
			const size_t fixed = sizeof(Page) + sizeof(Entry) + 2U * sizeof(Key) + 8U * sizeof(void *);
			if (fixed > maximum_bytes_ || variableBytes > maximum_bytes_ - fixed) return;
			const Key identity = key<Page>(reference);
			if (entries_.contains(identity)) return;
			const size_t charge = fixed + variableBytes;
			while (!order_.empty() &&
				(entries_.size() == MAXIMUM_ENTRIES || charge > maximum_bytes_ - charged_bytes_)) {
				const auto found = entries_.find(order_.front());
				charged_bytes_ -= found->second.charge;
				entries_.erase(found);
				order_.pop_front();
				debug::PerfTrace::addValue("native_graph.read.decoded_metadata_cache_evictions", 1);
			}
			auto copy = std::make_shared<const Page>(page);
			order_.push_back(identity);
			try { entries_.emplace(identity, Entry{std::move(copy), charge}); }
			catch (...) { order_.pop_back(); throw; }
			charged_bytes_ += charge;
		}

		[[nodiscard]] size_t chargedBytes() const { std::lock_guard lock(mutex_); return charged_bytes_; }
		[[nodiscard]] size_t size() const { std::lock_guard lock(mutex_); return entries_.size(); }

	private:
		struct Key {
			PhysicalPageRef reference;
			std::type_index type;
			bool operator==(const Key &) const = default;
		};
		struct KeyHash {
			size_t operator()(const Key &key) const noexcept {
				return std::hash<uint64_t>{}(key.reference.physical_slot.value()) ^
					(std::hash<uint64_t>{}(key.reference.generation.value()) << 1U) ^
					(static_cast<size_t>(key.reference.frame_index) << 3U) ^ key.type.hash_code();
			}
		};
		struct Entry { std::shared_ptr<const void> page; size_t charge; };
		template<typename Page> static Key key(PhysicalPageRef ref) { return {ref, typeid(Page)}; }
		void synchronizeWrites() {
			const uint64_t epoch = io_->dataMutationEpoch();
			if (epoch != mutation_epoch_ || epoch == std::numeric_limits<uint64_t>::max()) {
				entries_.clear(); order_.clear(); charged_bytes_ = 0U;
				mutation_epoch_ = epoch;
			}
		}

		inline static thread_local MetadataPageReadContext *active_ = nullptr;
		Superblock snapshot_;
		const size_t maximum_bytes_;
		const StorageIO *io_ = nullptr;
		uint64_t mutation_epoch_ = 0U;
		mutable std::mutex mutex_;
		std::unordered_map<Key, Entry, KeyHash> entries_;
		std::deque<Key> order_;
		size_t charged_bytes_ = 0U;
	};
} // namespace graph::storage
