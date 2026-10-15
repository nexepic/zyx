/**
 * @file MetadataPageReadContext.hpp
 * @brief Bounded decoded metadata reuse owned by a live snapshot pin.
 */
#pragma once

#include <list>
#include <limits>
#include <stdexcept>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/query/execution/ResourceCoordinator.hpp"
#include <mutex>
#include <optional>
#include <typeindex>
#include <unordered_map>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/Superblock.hpp"

namespace graph::storage {
    struct CatalogLeafPage;
    struct CatalogRadixPage;
    struct AllocatorLeafPage;
    struct LocationLeafPage;
    struct MetadataPageRouteLeafPage;
    struct PageDirectoryBootstrap;

	// Only immutable, unresolved codec results belong here. Logical child
	// resolution and caller-specific owner/visibility checks remain outside.
	class MetadataPageReadContext final {
	public:
		static constexpr size_t MAXIMUM_ENTRIES = 128U;
		static constexpr size_t MAXIMUM_CHARGED_BYTES = 1024U * 1024U;

		explicit MetadataPageReadContext(Superblock snapshot,
				size_t maximumBytes = MAXIMUM_CHARGED_BYTES,
                std::shared_ptr<std::pmr::memory_resource> memoryOwner = {})
            : snapshot_(snapshot), maximum_bytes_(maximumBytes), memory_owner_(std::move(memoryOwner)) {
			static_cast<void>(SuperblockCodec::encode(snapshot_));
			if (maximumBytes > MAXIMUM_CHARGED_BYTES) throw std::invalid_argument("metadata cache exceeds its byte limit");
		}

		class Scope final {
		public:
			Scope(std::shared_ptr<MetadataPageReadContext> context, const StorageIO &io) :
				context_(std::move(context)), previous_(active_), previous_scope_(active_scope_) {
				if (!context_) throw std::invalid_argument("metadata read scope requires a live context");
				std::lock_guard lock(context_->mutex_);
				if (context_->io_ != nullptr && context_->io_ != &io) {
					throw std::invalid_argument("metadata read context cannot cross StorageIO identities");
				}
				if (context_->io_ == nullptr) context_->io_ = &io;
				context_->synchronizeWrites();
				active_ = context_.get();
				active_scope_ = this;
			}
			~Scope() { active_ = previous_; active_scope_ = previous_scope_; }
			Scope(const Scope &) = delete;
			Scope &operator=(const Scope &) = delete;
            [[nodiscard]] static std::shared_ptr<MetadataPageReadContext> currentOwner(const StorageIO &io) {
                return !StorageIO::SequentialReadScope::activeFor(io) && active_scope_ && active_scope_->context_->io_ == &io ? active_scope_->context_ : nullptr;
            }
		private:
			std::shared_ptr<MetadataPageReadContext> context_;
			MetadataPageReadContext *previous_;
            Scope *previous_scope_;
            inline static thread_local Scope *active_scope_ = nullptr;
		};

		[[nodiscard]] static MetadataPageReadContext *current(const StorageIO &io) {
			return !StorageIO::SequentialReadScope::activeFor(io) && active_ != nullptr && active_->io_ == &io ? active_ : nullptr;
		}
		[[nodiscard]] static MetadataPageReadContext *current(const StorageIO &io, const Superblock &view) {
			return !StorageIO::SequentialReadScope::activeFor(io) && active_ != nullptr && active_->io_ == &io &&
				active_->snapshot_.database_id == view.database_id &&
				active_->snapshot_.data_region_offset == view.data_region_offset ? active_ : nullptr;
		}
		[[nodiscard]] bool authenticates(const Superblock &context) const { return context == snapshot_; }

        template<typename Page>
        [[nodiscard]] std::shared_ptr<const Page> findView(PhysicalPageRef reference) {
            std::lock_guard lock(mutex_);
            synchronizeWrites();
            if (!cache_) return {};
            const auto found = cache_->entries.find(key<Page>(reference));
            if (found == cache_->entries.end()) return {};
            debug::PerfTrace::addValue("native_graph.read.decoded_metadata_cache_hits", 1);
            return std::static_pointer_cast<const Page>(found->second.page);
        }

		template<typename Page>
		[[nodiscard]] std::optional<Page> find(PhysicalPageRef reference) {
			std::lock_guard lock(mutex_);
			synchronizeWrites();
            if (!cache_) return std::nullopt;
			const auto found = cache_->entries.find(key<Page>(reference));
			if (found == cache_->entries.end()) return std::nullopt;
			debug::PerfTrace::addValue("native_graph.read.decoded_metadata_cache_hits", 1);
            utils::OwnedAllocationScope allocation(memory_owner_);
            try { return *std::static_pointer_cast<const Page>(found->second.page); }
            catch (const query::execution::detail::ResourceAdmissionDenied &) {
                // A cache copy may need a second payload allocation. Drop this
                // optional entry so the caller can decode one admitted copy.
                const Key identity = found->first;
                charged_bytes_ -= found->second.charge;
                cache_->entries.erase(found);
                std::erase(cache_->order, identity);
                return std::nullopt;
            }
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
            // Coordinated caches admit only the audited native codec pages.
            // Unknown page types retain the uncoordinated embedding contract.
            if constexpr (!(std::is_same_v<Page, CatalogLeafPage> || std::is_same_v<Page, CatalogRadixPage> ||
                            std::is_same_v<Page, AllocatorLeafPage> || std::is_same_v<Page, LocationLeafPage> ||
                            std::is_same_v<Page, MetadataPageRouteLeafPage> || std::is_same_v<Page, PageDirectoryBootstrap>)) {
                if (memory_owner_) return;
            }
            if (!cache_) {
                try { cache_.emplace(memory_owner_ ? memory_owner_.get() : std::pmr::get_default_resource()); }
                catch (const query::execution::detail::ResourceAdmissionDenied &) { return; }
            }
			const Key identity = key<Page>(reference);
			if (cache_->entries.contains(identity)) return;
			const size_t charge = fixed + variableBytes;
			while (!cache_->order.empty() &&
				(cache_->entries.size() == MAXIMUM_ENTRIES || charge > maximum_bytes_ - charged_bytes_)) {
				const auto found = cache_->entries.find(cache_->order.front());
				charged_bytes_ -= found->second.charge;
				cache_->entries.erase(found);
				cache_->order.pop_front();
				debug::PerfTrace::addValue("native_graph.read.decoded_metadata_cache_evictions", 1);
			}
            utils::OwnedAllocationScope allocation(memory_owner_);
            for (;;) {
                try {
                    auto copy = std::allocate_shared<const Page>(utils::OwnedPmrAllocator<Page>(memory_owner_), page);
                    cache_->order.push_back(identity);
                    try { cache_->entries.emplace(identity, Entry{std::move(copy), charge}); }
                    catch (...) { cache_->order.pop_back(); throw; }
                    charged_bytes_ += charge;
                    return;
                } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                    if (cache_->order.empty()) return;
                    const auto oldest = cache_->entries.find(cache_->order.front());
                    charged_bytes_ -= oldest->second.charge;
                    cache_->entries.erase(oldest);
                    cache_->order.pop_front();
                }
            }
		}

        [[nodiscard]] const std::shared_ptr<std::pmr::memory_resource> &memoryOwner() const noexcept { return memory_owner_; }
		[[nodiscard]] size_t chargedBytes() const { std::lock_guard lock(mutex_); return charged_bytes_; }
		[[nodiscard]] size_t size() const { std::lock_guard lock(mutex_); return cache_ ? cache_->entries.size() : 0U; }

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
				cache_.reset(); charged_bytes_ = 0U;
				mutation_epoch_ = epoch;
			}
		}

		inline static thread_local MetadataPageReadContext *active_ = nullptr;
		Superblock snapshot_;
		const size_t maximum_bytes_;
		const StorageIO *io_ = nullptr;
		uint64_t mutation_epoch_ = 0U;
		mutable std::mutex mutex_;
        std::shared_ptr<std::pmr::memory_resource> memory_owner_;
        struct PayloadCache {
            explicit PayloadCache(std::pmr::memory_resource *resource) : entries(resource), order(resource) {}
            std::pmr::unordered_map<Key, Entry, KeyHash> entries;
            std::pmr::list<Key> order;
        };
        // Some standard libraries allocate sentinel/proxy nodes even for empty
        // containers. Snapshot controls must never depend on CACHE headroom.
        std::optional<PayloadCache> cache_;
		size_t charged_bytes_ = 0U;
	};
} // namespace graph::storage
