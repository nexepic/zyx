/**
 * @file NativeReadSession.hpp
 * @brief Snapshot-bound, bounded state shared by native read adapters.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"
#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"

namespace graph::storage {

	// This session is query-owned and snapshot-bound. It carries no page offsets
	// or PageIds across the logical boundary. Entries use a bounded FIFO rather
	// than an ID modulo table so sequential scans do not collide.
	class NativeReadSession final {
	public:
		static constexpr size_t MAXIMUM_CACHED_ENTITY_RECORDS = 128U;
		static constexpr size_t MAXIMUM_CACHED_TOKEN_NAMES = 128U;
		static constexpr size_t MAXIMUM_CACHED_SCHEMA_DEFINITIONS = 64U;
		static constexpr size_t MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES = 512U * 1024U;
		static constexpr size_t MAXIMUM_CACHED_PROPERTIES = 512U;
		static constexpr size_t MAXIMUM_CACHED_NODE_LABEL_SETS = 256U;
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
				return found->second;
			}
			return std::nullopt;
		}
		void putNodeLabels(uint64_t id, const std::vector<uint64_t> &labels) const {
			if (id == 0U || labels.size() > MAXIMUM_CACHED_NODE_LABEL_SETS) {
				if (id == 0U) {
					throw std::invalid_argument("native read session Node label cache requires a nonzero identifier");
				}
				return;
			}
			for (size_t index = 0U; index < labels.size(); ++index) {
				if (labels[index] == 0U || (index != 0U && labels[index] <= labels[index - 1U])) {
					throw std::invalid_argument("native read session Node label cache requires ordered nonzero tokens");
				}
			}
			const auto found = node_label_cache_.find(id);
			if (found != node_label_cache_.end()) {
				found->second = labels;
				return;
			}
			if (node_label_cache_.size() == MAXIMUM_CACHED_NODE_LABEL_SETS) {
				node_label_cache_.erase(node_label_order_.front());
				node_label_order_.pop_front();
			}
			node_label_order_.push_back(id);
			node_label_cache_.emplace(id, labels);
		}
		void putNode(uint64_t id, const NodeEntityRecord &value) const {
			put(node_cache_, node_order_, id, value);
		}
		void putEdge(uint64_t id, const EdgeEntityRecord &value) const {
			put(edge_cache_, edge_order_, id, value);
		}

		// The outer optional distinguishes a cache miss from a cached absent value.
		// Property values are snapshot-bound through bind(), so this cache is safe
		// for repeated expression/projection reads within one query session.
		[[nodiscard]] std::optional<std::optional<PropertyValue>> property(
				bool isEdge, uint64_t entityId, uint32_t token) const {
			const auto found = property_cache_.find(PropertyCacheKey{isEdge, entityId, token});
			if (found == property_cache_.end()) {
				return std::nullopt;
			}
			return found->second.has_value() ? std::optional<std::optional<PropertyValue>>{found->second}
											 : std::optional<std::optional<PropertyValue>>{std::optional<PropertyValue>{}};
		}
		void putProperty(bool isEdge, uint64_t entityId, uint32_t token,
						 std::optional<PropertyValue> value) const {
			if (entityId == 0U || token == 0U) {
				throw std::invalid_argument("native read session Property cache requires nonzero identifiers");
			}
			const PropertyCacheKey key{isEdge, entityId, token};
			const size_t valueBytes = value.has_value() ? propertyValueBytes(*value) : 0U;
			if (valueBytes > MAXIMUM_CACHED_PROPERTY_BYTES) {
				// Do not retain an oversized value. Remove an older entry for the same
				// key so a stale cached value cannot survive a deliberate bypass.
				if (const auto found = property_cache_.find(key); found != property_cache_.end()) {
					property_cache_bytes_ -= found->second.has_value() ? propertyValueBytes(*found->second) : 0U;
					property_cache_.erase(found);
					std::erase(property_order_, key);
				}
				return;
			}
			if (const auto found = property_cache_.find(key); found != property_cache_.end()) {
				property_cache_bytes_ -= found->second.has_value() ? propertyValueBytes(*found->second) : 0U;
				found->second = std::move(value);
				property_cache_bytes_ += valueBytes;
				return;
			}
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
			property_order_.push_back(key);
			property_cache_.emplace(key, std::move(value));
			property_cache_bytes_ += valueBytes;
		}

		// Presence is cached separately from decoded values so COUNT(property) can
		// reuse an authenticated LargeValue/root probe without retaining its payload.
		[[nodiscard]] std::optional<bool> propertyPresence(bool isEdge, uint64_t entityId, uint32_t token) const {
			const auto found = property_presence_cache_.find(PropertyCacheKey{isEdge, entityId, token});
			return found == property_presence_cache_.end() ? std::nullopt : std::optional<bool>(found->second);
		}
		void putPropertyPresence(bool isEdge, uint64_t entityId, uint32_t token, bool present) const {
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
			property_presence_cache_.emplace(key, present);
		}

		[[nodiscard]] std::optional<std::string> tokenName(TokenNameKind kind, uint64_t tokenId) const {
			const auto found = token_name_cache_.find(TokenNameKey{kind, tokenId});
			if (found != token_name_cache_.end()) {
				return found->second;
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
		void putPropertyToken(std::string name, std::optional<uint32_t> token) const {
			if (name.empty() || (token.has_value() && *token == 0U)) {
				throw std::invalid_argument("native read session Property token cache requires a name and nonzero token");
			}
			const uint32_t stored = token.value_or(0U);
			const auto found = property_token_cache_.find(name);
			if (found != property_token_cache_.end()) {
				found->second = stored;
				return;
			}
			if (property_token_cache_.size() == MAXIMUM_CACHED_TOKEN_NAMES) {
				property_token_cache_.erase(property_token_order_.front());
				property_token_order_.pop_front();
			}
			property_token_order_.push_back(name);
			property_token_cache_.emplace(std::move(name), stored);
		}
		[[nodiscard]] std::optional<std::optional<uint64_t>> nodeLabelToken(std::string_view name) const {
			const auto found = node_label_token_cache_.find(name);
			if (found == node_label_token_cache_.end()) {
				return std::nullopt;
			}
			return found->second == 0U ? std::optional<std::optional<uint64_t>>{std::optional<uint64_t>{}}
													 : std::optional<std::optional<uint64_t>>{found->second};
		}
		void putNodeLabelToken(std::string name, std::optional<uint64_t> token) const {
			if (name.empty() || (token.has_value() && *token == 0U)) {
				throw std::invalid_argument("native read session Node-label token cache requires a name and nonzero token");
			}
			const uint64_t stored = token.value_or(0U);
			const auto found = node_label_token_cache_.find(name);
			if (found != node_label_token_cache_.end()) {
				found->second = stored;
				return;
			}
			if (node_label_token_cache_.size() == MAXIMUM_CACHED_TOKEN_NAMES) {
				node_label_token_cache_.erase(node_label_token_order_.front());
				node_label_token_order_.pop_front();
			}
			node_label_token_order_.push_back(name);
			node_label_token_cache_.emplace(std::move(name), stored);
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
		void putRelationshipTypeToken(std::string name, std::optional<uint64_t> token) const {
			if (name.empty() || (token.has_value() && *token == 0U)) {
				throw std::invalid_argument(
						"native read session relationship-type token cache requires a name and nonzero token");
			}
			const uint64_t stored = token.value_or(0U);
			const auto found = relationship_type_token_cache_.find(name);
			if (found != relationship_type_token_cache_.end()) {
				found->second = stored;
				return;
			}
			if (relationship_type_token_cache_.size() == MAXIMUM_CACHED_TOKEN_NAMES) {
				relationship_type_token_cache_.erase(relationship_type_token_order_.front());
				relationship_type_token_order_.pop_front();
			}
			relationship_type_token_order_.push_back(name);
			relationship_type_token_cache_.emplace(std::move(name), stored);
		}
		void putTokenName(TokenNameKind kind, uint64_t tokenId, std::string name) const {
			if (tokenId == 0U || name.empty()) {
				throw std::invalid_argument("native read session token names require nonzero IDs and values");
			}
			const TokenNameKey key{kind, tokenId};
			const auto found = token_name_cache_.find(key);
			if (found != token_name_cache_.end()) {
				found->second = std::move(name);
				return;
			}
			if (token_name_cache_.size() == MAXIMUM_CACHED_TOKEN_NAMES) {
				const TokenNameKey evicted = token_name_order_.front();
				token_name_order_.pop_front();
				token_name_cache_.erase(evicted);
			}
			token_name_order_.push_back(key);
			token_name_cache_.emplace(key, std::move(name));
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
		void putIndexDefinitions(std::vector<IndexDefinition> definitions) const {
			if (fitsSchemaDefinitionCache(definitions)) {
				index_definitions_ = std::move(definitions);
			}
		}
		void putConstraintDefinitions(std::vector<ConstraintDefinition> definitions) const {
			if (fitsSchemaDefinitionCache(definitions)) {
				constraint_definitions_ = std::move(definitions);
			}
		}

		// Catalog entries are snapshot-stable. Retaining this bounded pool list
		// removes repeated Catalog traversals when one query opens several cursors.
		[[nodiscard]] const std::vector<CatalogEntry> *entityPagePools() const noexcept {
			return entity_page_pools_.has_value() ? &*entity_page_pools_ : nullptr;
		}
		void putEntityPagePools(const std::vector<CatalogEntry> &pools) const {
			if (pools.size() <= MAXIMUM_CACHED_ENTITY_PAGE_POOLS) {
				entity_page_pools_ = pools;
			}
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
								return stored.size() > MAXIMUM_CACHED_PROPERTY_BYTES ? OVER_BUDGET : stored.size();
							} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
								if (stored.size() > MAXIMUM_CACHED_PROPERTY_BYTES / sizeof(PropertyValue)) {
									return OVER_BUDGET;
								}
								size_t bytes = stored.size() * sizeof(PropertyValue);
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
								for (const auto &[key, item]: stored) {
									const size_t valueBytes = self(self, item, depth + 1U);
									if (valueBytes > MAXIMUM_CACHED_PROPERTY_BYTES) {
										return OVER_BUDGET;
									}
									if (key.size() > MAXIMUM_CACHED_PROPERTY_BYTES -
											valueBytes) {
										return OVER_BUDGET;
									}
									const size_t itemBytes = key.size() + valueBytes;
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
			size_t bytes = 0U;
			for (const Definition &definition: definitions) {
				size_t entryBytes = definition.name.size();
				for (const std::string &propertyName: definition.property_names) {
					if (entryBytes > std::numeric_limits<size_t>::max() - propertyName.size()) {
						return false;
					}
					entryBytes += propertyName.size();
				}
				if (entryBytes > MAXIMUM_CACHED_SCHEMA_DEFINITION_BYTES - bytes) {
					return false;
				}
				bytes += entryBytes;
			}
			return true;
		}

		template <typename T>
		static void put(std::unordered_map<uint64_t, T> &cache, std::deque<uint64_t> &order, uint64_t id,
							const T &value) {
			const auto found = cache.find(id);
			if (found != cache.end()) {
				found->second = value;
				return;
			}
			if (cache.size() == MAXIMUM_CACHED_ENTITY_RECORDS) {
				cache.erase(order.front());
				order.pop_front();
			}
			order.push_back(id);
			cache.emplace(id, value);
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

		mutable uint64_t generation_ = 0U;
		mutable uint64_t database_id_ = 0U;
		mutable bool bound_ = false;
		mutable std::unordered_map<uint64_t, NodeEntityRecord> node_cache_;
		mutable std::unordered_map<uint64_t, EdgeEntityRecord> edge_cache_;
		mutable std::unordered_map<uint64_t, bool> node_existence_cache_;
		mutable std::unordered_map<uint64_t, std::vector<uint64_t>> node_label_cache_;
		mutable std::deque<uint64_t> node_order_;
		mutable std::deque<uint64_t> edge_order_;
		mutable std::deque<uint64_t> node_existence_order_;
		mutable std::deque<uint64_t> node_label_order_;
		mutable std::unordered_map<PropertyCacheKey, std::optional<PropertyValue>, PropertyCacheKeyHash>
				property_cache_;
		mutable std::unordered_map<PropertyCacheKey, bool, PropertyCacheKeyHash> property_presence_cache_;
		mutable std::deque<PropertyCacheKey> property_order_;
		mutable std::deque<PropertyCacheKey> property_presence_order_;
		mutable size_t property_cache_bytes_ = 0U;
		mutable std::unordered_map<TokenNameKey, std::string, TokenNameKeyHash> token_name_cache_;
		mutable std::deque<TokenNameKey> token_name_order_;
		mutable std::unordered_map<std::string, uint32_t, TransparentStringHash, std::equal_to<>> property_token_cache_;
		mutable std::deque<std::string> property_token_order_;
		mutable std::unordered_map<std::string, uint64_t, TransparentStringHash, std::equal_to<>> node_label_token_cache_;
		mutable std::deque<std::string> node_label_token_order_;
		mutable std::unordered_map<std::string, uint64_t, TransparentStringHash, std::equal_to<>>
				relationship_type_token_cache_;
		mutable std::deque<std::string> relationship_type_token_order_;
		mutable std::optional<std::vector<IndexDefinition>> index_definitions_;
		mutable std::optional<std::vector<ConstraintDefinition>> constraint_definitions_;
		mutable std::optional<std::vector<CatalogEntry>> entity_page_pools_;
		mutable const PropertyCollectionRootStore *property_store_ = nullptr;
		mutable std::optional<PropertyCollectionRootScanCursor> property_cursor_;
	};

} // namespace graph::storage
