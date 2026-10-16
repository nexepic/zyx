/**
 * @file NativeReadSession.hpp
 * @brief Snapshot-bound, bounded state shared by native read adapters.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#pragma once

#include "graph/storage/detail/ScopedPropertyBuffers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/utils/OwnedPmrObject.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"
#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"

namespace graph::storage {

	// This session is query-owned and snapshot-bound. It carries no page offsets
	// or PageIds across the logical boundary. Entries use a bounded FIFO rather
	// than an ID modulo table so sequential scans do not collide.
	class NativeReadSession final : public utils::OwnedPmrObject {
	public:
		explicit NativeReadSession(std::shared_ptr<std::pmr::memory_resource> memory = {})
			: memory_(std::move(memory)),
			node_cache_(memoryResource()),
			edge_cache_(memoryResource()),
			node_existence_cache_(memoryResource()),
			node_label_cache_(memoryResource()),
			node_order_(memoryResource()),
			edge_order_(memoryResource()),
			node_existence_order_(memoryResource()),
			node_label_order_(memoryResource()),
			property_cache_(memoryResource()),
			owned_property_cache_(memoryResource()),
			property_presence_cache_(memoryResource()),
			property_order_(memoryResource()),
			property_presence_order_(memoryResource()),
			token_name_cache_(memoryResource()),
			token_name_order_(memoryResource()),
			property_token_cache_(memoryResource()),
			property_token_order_(memoryResource()),
			node_label_token_cache_(memoryResource()),
			node_label_token_order_(memoryResource()),
			relationship_type_token_cache_(memoryResource()),
			relationship_type_token_order_(memoryResource()) {}

        explicit NativeReadSession(const MetadataSnapshotPin &snapshot)
            : NativeReadSession(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
                (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr)) { bind(snapshot); }

		NativeReadSession(const NativeReadSession &) = delete;
		NativeReadSession &operator=(const NativeReadSession &) = delete;
		NativeReadSession(NativeReadSession &&) = delete;
		NativeReadSession &operator=(NativeReadSession &&) = delete;

		[[nodiscard]] const std::shared_ptr<std::pmr::memory_resource> &memoryOwner() const noexcept { return memory_; }

        static constexpr size_t MAXIMUM_CACHED_ENTITY_RECORDS = 128U;
		static constexpr size_t MAXIMUM_CACHED_TOKEN_NAMES = 128U;
		// Per namespace, including both copies of name-to-token keys (map/FIFO).
		// This bounds logical string payload, not allocator overhead or query RSS.
		static constexpr size_t MAXIMUM_CACHED_TOKEN_NAME_BYTES = 512U * 1024U;
		static constexpr size_t MAXIMUM_CACHED_SCHEMA_DEFINITIONS = 64U;
		static constexpr size_t MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES = 512U * 1024U;
		static constexpr size_t MAXIMUM_CACHED_PROPERTIES = 512U;
		static constexpr size_t MAXIMUM_CACHED_NODE_LABEL_SETS = 256U;
		static constexpr size_t MAXIMUM_CACHED_NODE_LABEL_BYTES = 64U * 1024U;
		// Property values may own strings, lists, maps, or binary payloads. Keep
		// the entry cap and add a byte cap so one large value cannot consume an
		// unbounded fraction of a query session's memory.
		static constexpr size_t MAXIMUM_CACHED_PROPERTY_BYTES = 4U * 1024U * 1024U;
		static constexpr size_t MAXIMUM_CACHED_ENTITY_PAGE_POOLS = 4'096U;

		// Prefixes keep this internal enum safe from Windows macro collisions.
		enum class TokenNameKind : uint8_t {
			TNK_NODE_LABEL = 1,
			TNK_RELATIONSHIP_TYPE = 2,
			TNK_PROPERTY_KEY = 3,
		};

		void bind(const MetadataSnapshotPin &snapshot) const {
			if (!snapshot.isValid()) {
				throw std::invalid_argument("native read session requires a live snapshot");
			}
			const uint64_t generation = snapshot.superblock().generation.value();
			const uint64_t databaseId = snapshot.superblock().database_id.value();
			if (!bound_) {
				generation_ = generation;
				database_id_ = databaseId;
				bound_ = true;
			} else if (generation != generation_ || databaseId != database_id_) {
				throw std::invalid_argument("native read session cannot cross database snapshots or metadata generations");
			}
		}

		[[nodiscard]] std::optional<NodeEntityRecord> node(uint64_t id) const {
			const auto found = node_cache_.find(id);
			if (found != node_cache_.end()) {
				return found->second;
			}
			return std::nullopt;
		}
		// Existence probes are common in topology-only traversals. Cache both
		// present and absent results within this snapshot so repeated endpoints do
		// not redo the Entity Directory descent, while keeping the same bounded
		// FIFO policy as record caches.
		[[nodiscard]] std::optional<bool> nodeExists(uint64_t id) const {
			const auto found = node_existence_cache_.find(id);
			return found == node_existence_cache_.end() ? std::nullopt : std::optional<bool>(found->second);
		}
		void putNodeExists(uint64_t id, bool exists) const {
			if (id == 0U) {
				throw std::invalid_argument("native read session Node existence cache requires a nonzero identifier");
			}
			put(node_existence_cache_, node_existence_order_, id, exists);
		}
		[[nodiscard]] std::optional<EdgeEntityRecord> edge(uint64_t id) const {
			const auto found = edge_cache_.find(id);
			if (found != edge_cache_.end()) {
				return found->second;
			}
			return std::nullopt;
		}
		[[nodiscard]] std::optional<std::vector<uint64_t>> nodeLabels(uint64_t id) const {
			const auto found = node_label_cache_.find(id);
			if (found != node_label_cache_.end()) {
				return std::vector<uint64_t>(found->second.begin(), found->second.end());
			}
			return std::nullopt;
		}
		// Borrow only until the next session mutation, clear, or snapshot bind.
		[[nodiscard]] std::optional<std::span<const uint64_t>> nodeLabelsView(uint64_t id) const {
			const auto found = node_label_cache_.find(id);
			if (found == node_label_cache_.end()) return std::nullopt;
			return std::span<const uint64_t>(found->second);
		}
		void putNodeLabels(uint64_t id, const std::vector<uint64_t> &labels) const try {
			const auto discardPrevious = [&]() {
				if (const auto found = node_label_cache_.find(id); found != node_label_cache_.end()) {
					node_label_cache_bytes_ -= found->second.capacity() * sizeof(uint64_t);
					node_label_cache_.erase(found);
					std::erase(node_label_order_, id);
				}
			};
			if (id == 0U || labels.size() > MAXIMUM_CACHED_NODE_LABEL_BYTES / sizeof(uint64_t)) {
				if (id == 0U) {
					throw std::invalid_argument("native read session Node label cache requires a nonzero identifier");
				}
				discardPrevious();
				return;
			}
			for (size_t index = 0U; index < labels.size(); ++index) {
				if (labels[index] == 0U || (index != 0U && labels[index] <= labels[index - 1U])) {
					throw std::invalid_argument("native read session Node label cache requires ordered nonzero tokens");
				}
			}
			std::pmr::vector<uint64_t> replacement(labels.begin(), labels.end(), memoryResource());
			const size_t bytes = replacement.capacity() * sizeof(uint64_t);
			if (bytes > MAXIMUM_CACHED_NODE_LABEL_BYTES) { discardPrevious(); return; }
			const auto evictOldest = [&]() {
				const auto oldest = node_label_cache_.find(node_label_order_.front());
				node_label_cache_bytes_ -= oldest->second.capacity() * sizeof(uint64_t);
				node_label_cache_.erase(oldest);
				node_label_order_.pop_front();
			};
			if (const auto found = node_label_cache_.find(id); found != node_label_cache_.end()) {
				node_label_cache_bytes_ -= found->second.capacity() * sizeof(uint64_t);
				found->second.swap(replacement);
				node_label_cache_bytes_ += bytes;
			} else {
				while (!node_label_order_.empty() &&
					(node_label_cache_.size() >= MAXIMUM_CACHED_NODE_LABEL_SETS ||
					 bytes > MAXIMUM_CACHED_NODE_LABEL_BYTES - node_label_cache_bytes_)) evictOldest();
				node_label_order_.push_back(id);
				try {
					node_label_cache_.emplace(id, std::move(replacement));
				} catch (...) {
					node_label_order_.pop_back();
					throw;
				}
				node_label_cache_bytes_ += bytes;
			}
			while (node_label_cache_bytes_ > MAXIMUM_CACHED_NODE_LABEL_BYTES) evictOldest();
		} catch (const query::execution::detail::ResourceAdmissionDenied &) {
            // Optional retention must not invalidate an authenticated read.
        }
		void putNode(uint64_t id, const NodeEntityRecord &value) const {
			put(node_cache_, node_order_, id, value);
		}
		void putEdge(uint64_t id, const EdgeEntityRecord &value) const {
			put(edge_cache_, edge_order_, id, value);
		}

		// Test presence without copying a potentially large cached payload.
		[[nodiscard]] bool hasProperty(bool isEdge, uint64_t entityId, uint32_t token) const {
			return memory_ ? owned_property_cache_.contains(PropertyCacheKey{isEdge, entityId, token})
				: property_cache_.contains(PropertyCacheKey{isEdge, entityId, token});
		}

		// The outer optional distinguishes a cache miss from a cached absent value.
		// Property values are snapshot-bound through bind(), so this cache is safe
		// for repeated expression/projection reads within one query session.
		[[nodiscard]] std::optional<std::optional<PropertyValue>> property(
				bool isEdge, uint64_t entityId, uint32_t token,
				const SelectedPropertyReadAdmission *admission = nullptr) const {
            if (memory_) {
                const auto found = owned_property_cache_.find(PropertyCacheKey{isEdge, entityId, token});
                if (found == owned_property_cache_.end()) return std::nullopt;
                if (!found->second) return std::optional<std::optional<PropertyValue>>{std::optional<PropertyValue>{}};
                if (admission) (*admission)(found->second->size() + sizeof(uint32_t));
                size_t offset = 0U;
                auto value = CanonicalPropertyValueCodec::decodeNext(*found->second, offset);
                if (offset != found->second->size()) throw std::logic_error("cached Property envelope has trailing bytes");
                return std::optional<std::optional<PropertyValue>>{std::move(value)};
            }
			const auto found = property_cache_.find(PropertyCacheKey{isEdge, entityId, token});
			if (found == property_cache_.end()) {
				return std::nullopt;
			}
			if (admission != nullptr && found->second.has_value()) {
				const size_t valueBytes = CanonicalPropertyValueCodec::encodedSize(*found->second);
				if (valueBytes > std::numeric_limits<size_t>::max() - sizeof(uint32_t)) {
					throw std::overflow_error("cached Property entry size overflowed");
				}
				(*admission)(valueBytes + sizeof(uint32_t));
			}
			return found->second.has_value() ? std::optional<std::optional<PropertyValue>>{found->second}
											 : std::optional<std::optional<PropertyValue>>{std::optional<PropertyValue>{}};
		}
		// Borrow lvalues until cache admission; consume rvalues only if retained.
		void putProperty(bool isEdge, uint64_t entityId, uint32_t token,
				const std::optional<PropertyValue> &value) const {
			putPropertyImpl(isEdge, entityId, token, value);
		}
		void putProperty(bool isEdge, uint64_t entityId, uint32_t token,
				std::optional<PropertyValue> &&value) const {
			putPropertyImpl(isEdge, entityId, token, std::move(value));
		}

		// Avoid an implicit optional payload copy/move before cache admission.
		void putProperty(bool isEdge, uint64_t entityId, uint32_t token, const PropertyValue &value) const {
			putPropertyImpl(isEdge, entityId, token, value);
		}
		void putProperty(bool isEdge, uint64_t entityId, uint32_t token, PropertyValue &&value) const {
			putPropertyImpl(isEdge, entityId, token, std::move(value));
		}

		// Presence is cached separately from decoded values so COUNT(property) can
		// reuse an authenticated LargeValue/root probe without retaining its payload.
		[[nodiscard]] std::optional<bool> propertyPresence(bool isEdge, uint64_t entityId, uint32_t token) const {
			const auto found = property_presence_cache_.find(PropertyCacheKey{isEdge, entityId, token});
			return found == property_presence_cache_.end() ? std::nullopt : std::optional<bool>(found->second);
		}
		void putPropertyPresence(bool isEdge, uint64_t entityId, uint32_t token, bool present) const try {
			if (entityId == 0U || token == 0U) {
				throw std::invalid_argument("native read session Property presence cache requires nonzero identifiers");
			}
			const PropertyCacheKey key{isEdge, entityId, token};
			if (const auto found = property_presence_cache_.find(key); found != property_presence_cache_.end()) {
				found->second = present;
				return;
			}
			if (property_presence_order_.size() >= MAXIMUM_CACHED_PROPERTIES) {
				property_presence_cache_.erase(property_presence_order_.front());
				property_presence_order_.pop_front();
			}
			property_presence_order_.push_back(key);
			try { property_presence_cache_.emplace(key, present); }
			catch (...) { property_presence_order_.pop_back(); throw; }
		} catch (const query::execution::detail::ResourceAdmissionDenied &) {
            // Optional retention must not invalidate an authenticated read.
        }

		[[nodiscard]] std::optional<std::string> tokenName(TokenNameKind kind, uint64_t tokenId,
				const std::function<void(size_t)> &copyAdmission = {}) const {
			const auto found = token_name_cache_.find(TokenNameKey{kind, tokenId});
			if (found != token_name_cache_.end()) {
				// Admit the owned copy before its string allocation; rejection leaves
				// the snapshot-local cached name and its retention budget intact.
				if (copyAdmission) copyAdmission(found->second.capacity());
                else detail::admitScopedPropertyString(found->second.size());
				return std::string(found->second);
			}
			return std::nullopt;
		}

		// The outer optional distinguishes an uncached name from a cached catalog
		// miss. Misses are retained because a predicate over an unknown Property
		// must not descend the Catalog again for every scanned row.
		[[nodiscard]] std::optional<std::optional<uint32_t>> propertyToken(std::string_view name) const {
			const auto found = property_token_cache_.find(name);
			if (found == property_token_cache_.end()) {
				return std::nullopt;
			}
			return found->second == 0U ? std::optional<std::optional<uint32_t>>{std::optional<uint32_t>{}}
											 : std::optional<std::optional<uint32_t>>{found->second};
		}
		void putPropertyToken(std::string_view name, std::optional<uint32_t> token) const {
			if (name.empty() || (token.has_value() && *token == 0U)) {
				throw std::invalid_argument("native read session Property token cache requires a name and nonzero token");
			}
			putNamedToken(property_token_cache_, property_token_order_, property_token_bytes_, name, token.value_or(0U));
		}
		[[nodiscard]] std::optional<std::optional<uint64_t>> nodeLabelToken(std::string_view name) const {
			const auto found = node_label_token_cache_.find(name);
			if (found == node_label_token_cache_.end()) {
				return std::nullopt;
			}
			return found->second == 0U ? std::optional<std::optional<uint64_t>>{std::optional<uint64_t>{}}
													 : std::optional<std::optional<uint64_t>>{found->second};
		}
		void putNodeLabelToken(std::string_view name, std::optional<uint64_t> token) const {
			if (name.empty() || (token.has_value() && *token == 0U)) {
				throw std::invalid_argument("native read session Node-label token cache requires a name and nonzero token");
			}
			putNamedToken(node_label_token_cache_, node_label_token_order_, node_label_token_bytes_, name, token.value_or(0U));
		}
		// Relationship-type names are resolved repeatedly by topology-only
		// operators. Cache both hits and misses in the same snapshot-bound,
		// bounded namespace as label and Property tokens.
		[[nodiscard]] std::optional<std::optional<uint64_t>> relationshipTypeToken(std::string_view name) const {
			const auto found = relationship_type_token_cache_.find(name);
			if (found == relationship_type_token_cache_.end()) {
				return std::nullopt;
			}
			return found->second == 0U ? std::optional<std::optional<uint64_t>>{std::optional<uint64_t>{}}
													 : std::optional<std::optional<uint64_t>>{found->second};
		}
		void putRelationshipTypeToken(std::string_view name, std::optional<uint64_t> token) const {
			if (name.empty() || (token.has_value() && *token == 0U)) {
				throw std::invalid_argument(
						"native read session relationship-type token cache requires a name and nonzero token");
			}
			putNamedToken(relationship_type_token_cache_, relationship_type_token_order_, relationship_type_token_bytes_, name, token.value_or(0U));
		}
		void putTokenName(TokenNameKind kind, uint64_t tokenId, std::string_view name) const try {
			if (tokenId == 0U || name.empty()) {
				throw std::invalid_argument("native read session token names require nonzero IDs and values");
			}
			const TokenNameKey key{kind, tokenId};
			const auto found = token_name_cache_.find(key);
			const size_t oldBytes = found == token_name_cache_.end() ? 0U : found->second.size();
			if (name.size() > MAXIMUM_CACHED_TOKEN_NAME_BYTES) {
				if (found != token_name_cache_.end()) {
					token_name_bytes_ -= oldBytes;
					token_name_cache_.erase(found);
					std::erase(token_name_order_, key);
				}
				return;
			}
			while ((found == token_name_cache_.end() && token_name_cache_.size() >= MAXIMUM_CACHED_TOKEN_NAMES) ||
				   token_name_bytes_ - oldBytes > MAXIMUM_CACHED_TOKEN_NAME_BYTES - name.size()) {
				// Preserve the original FIFO position of an updated key.
				const auto evicted = std::find_if(token_name_order_.begin(), token_name_order_.end(),
						[&key](const TokenNameKey &candidate) { return candidate != key; });
				const auto entry = token_name_cache_.find(*evicted);
				token_name_bytes_ -= entry->second.size();
				token_name_cache_.erase(entry);
				token_name_order_.erase(evicted);
			}
			if (found != token_name_cache_.end()) {
				found->second = name;
				token_name_bytes_ = token_name_bytes_ - oldBytes + name.size();
				return;
			}
			const auto inserted = token_name_cache_.emplace(key, name).first;
			try {
				token_name_order_.push_back(key);
			} catch (...) {
				token_name_cache_.erase(inserted);
				throw;
			}
			token_name_bytes_ += name.size();
		} catch (const query::execution::detail::ResourceAdmissionDenied &) {
            // Optional retention must not invalidate an authenticated read.
        }

		// Schema lists are immutable for a bound metadata generation. Cache only a
		// bounded validated copy, so repeated index candidates do not repeatedly
		// scan the System Catalog while a pathological catalog cannot grow a query.
		[[nodiscard]] const std::vector<IndexDefinition> *indexDefinitions(size_t maximumDefinitions) const {
			if (!index_definitions_.has_value() || index_definitions_->size() > maximumDefinitions) {
				return nullptr;
			}
			return &*index_definitions_;
		}
		[[nodiscard]] const std::vector<ConstraintDefinition> *
		constraintDefinitions(size_t maximumDefinitions) const {
			if (!constraint_definitions_.has_value() || constraint_definitions_->size() > maximumDefinitions) {
				return nullptr;
			}
			return &*constraint_definitions_;
		}
        void putIndexDefinitions(const std::vector<IndexDefinition> &definitions) const {
            if (!fitsSchemaDefinitionCache(definitions)) return;
            // Every field and backing copy selects CACHE, so a retained schema
            // list never keeps a finished query's private authority alive.
            if (memory_ && !std::dynamic_pointer_cast<query::execution::detail::PrepaidMemoryResource>(memory_)) return;
            try {
                utils::OwnedAllocationScope scope(memory_);
                utils::OwnedStdVector<IndexDefinition> copy(definitions);
                index_definitions_ = std::move(copy);
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                // This cache is optional; preserve its former validated list.
            }
        }
        void putConstraintDefinitions(const std::vector<ConstraintDefinition> &definitions) const {
            if (!fitsSchemaDefinitionCache(definitions)) return;
            if (memory_ && !std::dynamic_pointer_cast<query::execution::detail::PrepaidMemoryResource>(memory_)) return;
            try {
                utils::OwnedAllocationScope scope(memory_);
                utils::OwnedStdVector<ConstraintDefinition> copy(definitions);
                constraint_definitions_ = std::move(copy);
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                // Optional CACHE admission cannot invalidate the old list.
            }
        }

		// Catalog entries are snapshot-stable. Retaining this bounded pool list
		// removes repeated Catalog traversals when one query opens several cursors.
		[[nodiscard]] const std::pmr::vector<CatalogEntry> *entityPagePools() const noexcept {
			return entity_page_pools_.has_value() ? &*entity_page_pools_ : nullptr;
		}
		void putEntityPagePools(const std::vector<CatalogEntry> &pools) const try {
			if (pools.size() <= MAXIMUM_CACHED_ENTITY_PAGE_POOLS) {
				std::pmr::vector<CatalogEntry> replacement(pools.begin(), pools.end(), memoryResource());
				entity_page_pools_ = std::move(replacement);
			}
		} catch (const query::execution::detail::ResourceAdmissionDenied &) {
            // Optional retention must not invalidate an authenticated read.
        }

		// Selected Property reads share one snapshot-bound cursor. It retains at
		// most one decoded Property page and falls back to an authenticated point
		// lookup when the caller's owner order is not monotonic.
		[[nodiscard]] PropertyCollectionRootScanCursor &
		propertyCursor(const PropertyCollectionRootStore &store, const MetadataSnapshotPin &snapshot) const {
			bind(snapshot);
			if (property_store_ == nullptr) {
				property_store_ = &store;
				property_cursor_.emplace(store.openScanCursor(snapshot));
			} else if (property_store_ != &store) {
				throw std::invalid_argument("native read session cannot mix Property stores");
			}
			return *property_cursor_;
		}

	private:
		[[nodiscard]] std::pmr::memory_resource *memoryResource() const noexcept {
			return memory_ ? memory_.get() : std::pmr::get_default_resource();
		}
		template <typename Cache>
		static void putNamedToken(Cache &cache, std::pmr::deque<std::pmr::string> &order, size_t &bytes,
				std::string_view name, typename Cache::mapped_type token) try {
			if (const auto found = cache.find(name); found != cache.end()) {
				found->second = token;
				return;
			}
			// Check before constructing either retained name copy. Cache bypass
			// still permits the caller to use the resolved token for this read.
			if (name.size() > MAXIMUM_CACHED_TOKEN_NAME_BYTES / 2U) return;
			const size_t nameBytes = name.size() * 2U;
			while (!order.empty() && (cache.size() >= MAXIMUM_CACHED_TOKEN_NAMES ||
					bytes > MAXIMUM_CACHED_TOKEN_NAME_BYTES - nameBytes)) {
				bytes -= order.front().size() * 2U;
				cache.erase(order.front());
				order.pop_front();
			}
			const auto inserted = cache.emplace(name, token).first;
			try {
				order.push_back(inserted->first);
			} catch (...) {
				cache.erase(inserted);
				throw;
			}
			bytes += nameBytes;
		} catch (const query::execution::detail::ResourceAdmissionDenied &) {
            // Optional retention must not invalidate an authenticated read.
        }

        template<class Value>
        void putOwnedProperty(bool isEdge, uint64_t entityId, uint32_t token, const Value &input) const try {
            const PropertyValue *value = [&]() -> const PropertyValue * {
                if constexpr (std::is_same_v<std::remove_cvref_t<Value>, PropertyValue>) return &input;
                else return input ? &*input : nullptr;
            }();
            const PropertyCacheKey key{isEdge, entityId, token};
            const auto eraseEntry = [&](const PropertyCacheKey &entry) {
                const auto found = owned_property_cache_.find(entry);
                if (found == owned_property_cache_.end()) return;
                property_cache_bytes_ -= found->second ? found->second->capacity() : 0U;
                owned_property_cache_.erase(found);
            };
            // Preserve the legacy input-capacity safeguard before any clone or
            // encoding allocation. Coordinated retention uses canonical bytes,
            // so nested std::string/list/map heaps cannot escape the arena.
            if (value && propertyValueBytes(*value) > MAXIMUM_CACHED_PROPERTY_BYTES) {
                eraseEntry(key); std::erase(property_order_, key); return;
            }
            const size_t encoded = value ? CanonicalPropertyValueCodec::encodedSize(*value) : 0U;
            if (encoded > MAXIMUM_CACHED_PROPERTY_BYTES) {
                eraseEntry(key); std::erase(property_order_, key); return;
            }
            std::optional<std::pmr::vector<std::byte>> replacement;
            if (value) {
                replacement.emplace(memoryResource());
                replacement->reserve(encoded);
                CanonicalPropertyValueCodec::append(*replacement, *value);
            }
            const size_t bytes = replacement ? replacement->capacity() : 0U;
            if (bytes > MAXIMUM_CACHED_PROPERTY_BYTES) return;
            // Complete encoding before changing either cache index. No input
            // rvalue is consumed on a denied allocation.
            if (auto found = owned_property_cache_.find(key); found != owned_property_cache_.end()) {
                const size_t old = found->second ? found->second->capacity() : 0U;
                found->second = std::move(replacement);
                property_cache_bytes_ = property_cache_bytes_ - old + bytes;
            } else {
                property_order_.push_back(key);
                try { owned_property_cache_.emplace(key, std::move(replacement)); }
                catch (...) { property_order_.pop_back(); throw; }
                property_cache_bytes_ += bytes;
            }
            while (owned_property_cache_.size() > MAXIMUM_CACHED_PROPERTIES ||
                   property_cache_bytes_ > MAXIMUM_CACHED_PROPERTY_BYTES) {
                eraseEntry(property_order_.front()); property_order_.pop_front();
            }
        } catch (const query::execution::detail::ResourceAdmissionDenied &) {
            // Optional retention must not invalidate an authenticated read.
        }

		template <typename Value>
		void putPropertyImpl(bool isEdge, uint64_t entityId, uint32_t token, Value &&value) const {
			if (entityId == 0U || token == 0U) {
				throw std::invalid_argument("native read session Property cache requires nonzero identifiers");
			}
			if (memory_) { putOwnedProperty(isEdge, entityId, token, value); return; }
			const PropertyCacheKey key{isEdge, entityId, token};
			const size_t valueBytes = [&] {
				if constexpr (std::is_same_v<std::remove_cvref_t<Value>, PropertyValue>) return propertyValueBytes(value);
				else return value.has_value() ? propertyValueBytes(*value) : 0U;
			}();
			// Remove the old payload before admitting its replacement. Updates share
			// the same retained-byte cap as new keys and refresh FIFO insertion order.
			if (const auto found = property_cache_.find(key); found != property_cache_.end()) {
				property_cache_bytes_ -= found->second.has_value() ? propertyValueBytes(*found->second) : 0U;
				property_cache_.erase(found);
				std::erase(property_order_, key);
			}
			if (valueBytes > MAXIMUM_CACHED_PROPERTY_BYTES) return;
			while (!property_order_.empty() &&
				   (property_cache_.size() >= MAXIMUM_CACHED_PROPERTIES ||
					property_cache_bytes_ > MAXIMUM_CACHED_PROPERTY_BYTES - valueBytes)) {
				const PropertyCacheKey evicted = property_order_.front();
				property_order_.pop_front();
				if (const auto found = property_cache_.find(evicted); found != property_cache_.end()) {
					property_cache_bytes_ -= found->second.has_value() ? propertyValueBytes(*found->second) : 0U;
					property_cache_.erase(found);
				}
			}
			const auto inserted = property_cache_.emplace(key, std::forward<Value>(value)).first;
			const size_t retainedBytes = inserted->second.has_value() ? propertyValueBytes(*inserted->second) : 0U;
			if (retainedBytes > MAXIMUM_CACHED_PROPERTY_BYTES) {
				property_cache_.erase(inserted);
				return;
			}
			while (!property_order_.empty() && property_cache_bytes_ > MAXIMUM_CACHED_PROPERTY_BYTES - retainedBytes) {
				const PropertyCacheKey evicted = property_order_.front();
				property_order_.pop_front();
				if (const auto found = property_cache_.find(evicted); found != property_cache_.end()) {
					property_cache_bytes_ -= found->second.has_value() ? propertyValueBytes(*found->second) : 0U;
					property_cache_.erase(found);
				}
			}
			try {
				property_order_.push_back(key);
			} catch (...) {
				property_cache_.erase(inserted);
				throw;
			}
			property_cache_bytes_ += retainedBytes;
		}

		[[nodiscard]] static size_t propertyValueBytes(const PropertyValue &value) {
			constexpr size_t MAX_DEPTH = 64U;
			constexpr size_t OVER_BUDGET = MAXIMUM_CACHED_PROPERTY_BYTES + 1U;
			const auto estimate = [](const auto &self, const PropertyValue &current, size_t depth) -> size_t {
				if (depth >= MAX_DEPTH) {
					return OVER_BUDGET;
				}
				return std::visit(
						[&self, depth](const auto &stored) -> size_t {
							using T = std::decay_t<decltype(stored)>;
							if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, PropertyValue::Bytes>) {
								return stored.capacity() > MAXIMUM_CACHED_PROPERTY_BYTES ? OVER_BUDGET : stored.capacity();
							} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
								if (stored.capacity() > MAXIMUM_CACHED_PROPERTY_BYTES / sizeof(PropertyValue)) {
									return OVER_BUDGET;
								}
								size_t bytes = stored.capacity() * sizeof(PropertyValue);
								for (const PropertyValue &item: stored) {
									const size_t itemBytes = self(self, item, depth + 1U);
									if (itemBytes > MAXIMUM_CACHED_PROPERTY_BYTES ||
											itemBytes > MAXIMUM_CACHED_PROPERTY_BYTES - bytes) {
										return OVER_BUDGET;
									}
									bytes += itemBytes;
								}
								return bytes;
							} else if constexpr (std::is_same_v<T, PropertyValue::MapType>) {
								if (stored.size() > MAXIMUM_CACHED_PROPERTY_BYTES /
											 sizeof(std::pair<const std::string, PropertyValue>)) {
									return OVER_BUDGET;
								}
								size_t bytes = stored.size() * sizeof(std::pair<const std::string, PropertyValue>);
								if (stored.bucket_count() > (MAXIMUM_CACHED_PROPERTY_BYTES - bytes) / sizeof(void *)) return OVER_BUDGET;
								bytes += stored.bucket_count() * sizeof(void *);
								for (const auto &[key, item]: stored) {
									const size_t valueBytes = self(self, item, depth + 1U);
									if (valueBytes > MAXIMUM_CACHED_PROPERTY_BYTES) {
										return OVER_BUDGET;
									}
									if (key.capacity() > MAXIMUM_CACHED_PROPERTY_BYTES -
											valueBytes) {
										return OVER_BUDGET;
									}
									const size_t itemBytes = key.capacity() + valueBytes;
									if (itemBytes > MAXIMUM_CACHED_PROPERTY_BYTES - bytes) {
										return OVER_BUDGET;
									}
									bytes += itemBytes;
								}
								return bytes;
							} else {
								return sizeof(T);
							}
						},
						current.getVariant());
			};
			return estimate(estimate, value, 0U);
		}


		template <typename Definition>
		[[nodiscard]] static bool fitsSchemaDefinitionCache(const std::vector<Definition> &definitions) {
			if (definitions.size() > MAXIMUM_CACHED_SCHEMA_DEFINITIONS) {
				return false;
			}
			if (definitions.capacity() > MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES / sizeof(Definition)) {
				return false;
			}
			size_t bytes = definitions.capacity() * sizeof(Definition);
			const auto admit = [&](size_t retained) {
				if (retained > MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES - bytes) return false;
				bytes += retained;
				return true;
			};
			for (const Definition &definition: definitions) {
				if (!admit(definition.name.capacity()) ||
						definition.property_names.capacity() > (MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES - bytes) / sizeof(std::string) ||
						!admit(definition.property_names.capacity() * sizeof(std::string))) return false;
				for (const std::string &propertyName: definition.property_names) {
					if (!admit(propertyName.capacity())) return false;
				}
				if constexpr (requires { definition.vector_metric; }) {
					if (!admit(definition.vector_metric.capacity())) return false;
				}
			}
			return true;
		}

		template <typename T>
		static void put(std::pmr::unordered_map<uint64_t, T> &cache, std::pmr::deque<uint64_t> &order, uint64_t id,
							const T &value) {
			const auto found = cache.find(id);
			if (found != cache.end()) {
				found->second = value;
				return;
			}
			// Commit the two cache indexes together. If either allocation fails,
			// leave the existing FIFO and entries intact instead of retaining a
			// stale order ID or evicting a useful entry before admission succeeds.
			try {
				const auto [inserted, didInsert] = cache.emplace(id, value);
				if (!didInsert) return;
				try {
					order.push_back(id);
				} catch (...) {
					cache.erase(inserted);
					throw;
				}
			} catch (const query::execution::detail::ResourceAdmissionDenied &) {
				// Caching is optional: exhausted credit must not fail the read.
				// Rollback above also covers a denied FIFO block after map insertion.
				return;
			}
			if (cache.size() > MAXIMUM_CACHED_ENTITY_RECORDS) {
				cache.erase(order.front());
				order.pop_front();
			}
		}

		struct TokenNameKey {
			TokenNameKind kind = TokenNameKind::TNK_NODE_LABEL;
			uint64_t token_id = 0U;
			constexpr bool operator==(const TokenNameKey &) const = default;
		};
		struct TokenNameKeyHash {
			size_t operator()(const TokenNameKey &key) const noexcept {
				return std::hash<uint64_t>{}(key.token_id) ^
						(std::hash<uint8_t>{}(static_cast<uint8_t>(key.kind)) << 1U);
			}
		};
		struct PropertyCacheKey {
			bool is_edge = false;
			uint64_t entity_id = 0U;
			uint32_t token = 0U;
			constexpr bool operator==(const PropertyCacheKey &) const = default;
		};
		struct PropertyCacheKeyHash {
			size_t operator()(const PropertyCacheKey &key) const noexcept {
				const size_t idHash = std::hash<uint64_t>{}(key.entity_id);
				const size_t tokenHash = std::hash<uint32_t>{}(key.token);
				return idHash ^ (tokenHash + static_cast<size_t>(0x9e3779b9U) + (idHash << 6U) + (idHash >> 2U)) ^
						(static_cast<size_t>(key.is_edge) << 1U);
			}
		};
		struct TransparentStringHash {
			using is_transparent = void;
			size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
			size_t operator()(const std::string &value) const noexcept { return (*this)(std::string_view(value)); }
		};

		// Declared before every borrowing PMR container, so destroyed last.
		std::shared_ptr<std::pmr::memory_resource> memory_;
		mutable uint64_t generation_ = 0U;
		mutable uint64_t database_id_ = 0U;
		mutable bool bound_ = false;
		mutable std::pmr::unordered_map<uint64_t, NodeEntityRecord> node_cache_;
		mutable std::pmr::unordered_map<uint64_t, EdgeEntityRecord> edge_cache_;
		mutable std::pmr::unordered_map<uint64_t, bool> node_existence_cache_;
		mutable std::pmr::unordered_map<uint64_t, std::pmr::vector<uint64_t>> node_label_cache_;
		mutable size_t node_label_cache_bytes_ = 0U;
		mutable std::pmr::deque<uint64_t> node_order_;
		mutable std::pmr::deque<uint64_t> edge_order_;
		mutable std::pmr::deque<uint64_t> node_existence_order_;
		mutable std::pmr::deque<uint64_t> node_label_order_;
		mutable std::pmr::unordered_map<PropertyCacheKey, std::optional<PropertyValue>, PropertyCacheKeyHash>
				property_cache_;
		mutable std::pmr::unordered_map<PropertyCacheKey, std::optional<std::pmr::vector<std::byte>>, PropertyCacheKeyHash> owned_property_cache_;
		mutable std::pmr::unordered_map<PropertyCacheKey, bool, PropertyCacheKeyHash> property_presence_cache_;
		mutable std::pmr::deque<PropertyCacheKey> property_order_;
		mutable std::pmr::deque<PropertyCacheKey> property_presence_order_;
		mutable size_t property_cache_bytes_ = 0U;
		mutable std::pmr::unordered_map<TokenNameKey, std::pmr::string, TokenNameKeyHash> token_name_cache_;
		mutable std::pmr::deque<TokenNameKey> token_name_order_;
		mutable size_t token_name_bytes_ = 0U;
		mutable std::pmr::unordered_map<std::pmr::string, uint32_t, TransparentStringHash, std::equal_to<>> property_token_cache_;
		mutable std::pmr::deque<std::pmr::string> property_token_order_;
		mutable size_t property_token_bytes_ = 0U;
		mutable std::pmr::unordered_map<std::pmr::string, uint64_t, TransparentStringHash, std::equal_to<>> node_label_token_cache_;
		mutable std::pmr::deque<std::pmr::string> node_label_token_order_;
		mutable size_t node_label_token_bytes_ = 0U;
		mutable std::pmr::unordered_map<std::pmr::string, uint64_t, TransparentStringHash, std::equal_to<>>
				relationship_type_token_cache_;
		mutable std::pmr::deque<std::pmr::string> relationship_type_token_order_;
		mutable size_t relationship_type_token_bytes_ = 0U;
		mutable std::optional<utils::OwnedStdVector<IndexDefinition>> index_definitions_;
		mutable std::optional<utils::OwnedStdVector<ConstraintDefinition>> constraint_definitions_;
		mutable std::optional<std::pmr::vector<CatalogEntry>> entity_page_pools_;
		mutable const PropertyCollectionRootStore *property_store_ = nullptr;
		mutable std::optional<PropertyCollectionRootScanCursor> property_cursor_;
	};

    static_assert(alignof(NativeReadSession) <= alignof(std::max_align_t));

    // Existing query sessions need no fallback construction or deque allocation.
    // Standalone adapter calls inherit the snapshot's allocation domain.
    inline NativeReadSession &selectNativeReadSession(const MetadataSnapshotPin &snapshot,
            NativeReadSession *session, std::optional<NativeReadSession> &fallback) {
        if (!session) {
            if (!fallback) fallback.emplace(snapshot);
            session = &*fallback;
        }
        session->bind(snapshot);
        return *session;
    }

} // namespace graph::storage
