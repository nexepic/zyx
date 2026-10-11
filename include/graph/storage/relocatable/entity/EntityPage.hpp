/**
 * @file EntityPage.hpp
 * @brief Fixed Node/Edge page codec for the native relocatable store.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelLayout.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_ENTITY_PAGE_MAGIC = 0x4550595AU; // Little-endian bytes: ZYPE
	// Version 4 adds the logical EntityId to every fixed slot. This lets scans
	// enumerate a page directly while the Entity Map remains the authoritative
	// point-lookup and ordered-resume structure.
	constexpr uint16_t RSS_ENTITY_PAGE_VERSION = 4;
	constexpr uint16_t RSS_ENTITY_PAGE_HEADER_BYTES = 72;
	constexpr uint16_t RSS_ENTITY_NODE_RECORD_BYTES = 48;
	constexpr uint16_t RSS_ENTITY_EDGE_RECORD_BYTES = 24;
	// Zero is outside the schema-token registry. It preserves the Cypher
	// distinction between an omitted relationship type and a named type without
	// coupling native Entity pages to the retired fixed-record representation.
	constexpr uint64_t RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN = 0U;

	// Prefixes avoid Windows macro collisions.
	enum class EntityPageRecordKind : uint16_t {
		EPRK_NODE = 1,
		EPRK_EDGE = 2,
	};

	// A Node carries a direct stable reference to its optional overflow label set.
	// The future label-set store may compact record bytes without changing this
	// logical reference, just as a Property slot reference survives page-local COW.
	struct EntityLabelOverflowReference {
		PageId page_id{};
		uint16_t slot_id = 0;
		uint32_t slot_generation = 0;

		[[nodiscard]] constexpr bool isEmpty() const {
			return !page_id.isValid() && slot_id == 0U && slot_generation == 0U;
		}
		[[nodiscard]] constexpr bool isValid() const { return page_id.isValid() && slot_generation != 0U; }
		constexpr auto operator<=>(const EntityLabelOverflowReference &) const = default;
	};

	// Node payloads contain no physical adjacency state. The fixed page's parallel
	// EntityId array enables local scans; point lookup and ordered resume still use
	// the Entity Map, and graph traversal uses the global Packed B+Tree.
	struct NodeEntityRecord {
		std::array<uint64_t, RSS_ENTITY_INLINE_LABEL_CAPACITY> inline_label_ids{};
		EntityLabelOverflowReference label_overflow;
		uint8_t inline_label_count = 0;

		constexpr bool operator==(const NodeEntityRecord &) const = default;
	};

	// Edge topology is authoritative here; the global Packed B+Tree is a derived
	// key-only access path and never requires rewriting peer Edge records.
	struct EdgeEntityRecord {
		uint64_t source_node_id = 0;
		uint64_t target_node_id = 0;
		uint64_t type_id = 0;

		constexpr bool operator==(const EdgeEntityRecord &) const = default;
	};

	struct IdentifiedNodeEntityRecord {
		uint64_t entity_id = 0;
		NodeEntityRecord record;
		constexpr bool operator==(const IdentifiedNodeEntityRecord &) const = default;
	};

	struct IdentifiedEdgeEntityRecord {
		uint64_t entity_id = 0;
		EdgeEntityRecord record;

		constexpr bool operator==(const IdentifiedEdgeEntityRecord &) const = default;
	};

	using EntityPageNodeVisitor =
			std::function<bool(uint64_t entityId, uint16_t slot, uint32_t slotGeneration,
							   const NodeEntityRecord &record)>;
	using EntityPageEdgeVisitor =
			std::function<bool(uint64_t entityId, uint16_t slot, uint32_t slotGeneration,
							   const EdgeEntityRecord &record)>;

	class NodeEntityRecordCodec final {
	public:
		using Bytes = std::array<std::byte, RSS_ENTITY_NODE_RECORD_BYTES>;

		[[nodiscard]] static Bytes encode(const NodeEntityRecord &record);
		[[nodiscard]] static NodeEntityRecord decode(std::span<const std::byte> bytes);
	};

	class EdgeEntityRecordCodec final {
	public:
		using Bytes = std::array<std::byte, RSS_ENTITY_EDGE_RECORD_BYTES>;

		[[nodiscard]] static Bytes encode(const EdgeEntityRecord &record);
		[[nodiscard]] static EdgeEntityRecord decode(std::span<const std::byte> bytes);
	};

	struct EntityPageHeader {
		PageId page_id{};
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		EntityPageRecordKind record_kind = EntityPageRecordKind::EPRK_NODE;
		uint16_t record_bytes = 0;
		uint16_t slot_capacity = 0;
		uint16_t live_count = 0;
		uint16_t bitmap_bytes = 0;

		constexpr bool operator==(const EntityPageHeader &) const = default;
	};

	class EntityPageHeaderCodec final {
	public:
		using Bytes = std::array<std::byte, RSS_ENTITY_PAGE_HEADER_BYTES>;

		[[nodiscard]] static Bytes encode(const EntityPageHeader &header);
		[[nodiscard]] static EntityPageHeader decode(std::span<const std::byte> bytes);
	};

	// Read-only counterpart to EntityPage. It borrows already pinned and
	// authenticated bytes, decodes only the requested fixed record, and cannot
	// be used by a COW writer.
	class EntityPageReadView final {
	public:
		[[nodiscard]] static EntityPageReadView open(const PageOwnerHeader &owner,
															  std::span<const std::byte> bytes);

		[[nodiscard]] EntityPageHeader header() const { return header_; }
		[[nodiscard]] std::optional<NodeEntityRecord> findNode(uint16_t slot,
															uint32_t expectedSlotGeneration) const;
		[[nodiscard]] std::optional<NodeEntityRecord> findNode(uint16_t slot, uint32_t expectedSlotGeneration,
															uint64_t expectedEntityId) const;
		[[nodiscard]] std::optional<EdgeEntityRecord> findEdge(uint16_t slot,
															uint32_t expectedSlotGeneration) const;
		[[nodiscard]] std::optional<EdgeEntityRecord> findEdge(uint16_t slot, uint32_t expectedSlotGeneration,
															uint64_t expectedEntityId) const;
		[[nodiscard]] uint64_t entityId(uint16_t slot) const;
		[[nodiscard]] uint32_t slotGeneration(uint16_t slot) const;
		[[nodiscard]] bool visitNodesUntil(size_t maximumRecords, const EntityPageNodeVisitor &visitor) const;
		[[nodiscard]] bool visitEdgesUntil(size_t maximumRecords, const EntityPageEdgeVisitor &visitor) const;

	private:
		EntityPageReadView(std::span<const std::byte> bytes, EntityPageHeader header) :
				bytes_(bytes), header_(std::move(header)) {}

		void validateLayout() const;
		void validateLiveSlot(uint16_t slot) const;
		void validateSlotGeneration(uint16_t slot, uint32_t expectedSlotGeneration) const;
		[[nodiscard]] size_t bitmapOffset() const;
		[[nodiscard]] size_t slotGenerationOffset(uint16_t slot) const;
		[[nodiscard]] size_t entityIdOffset(uint16_t slot) const;
		[[nodiscard]] size_t recordOffset(uint16_t slot) const;
		[[nodiscard]] bool isLive(uint16_t slot) const;

		std::span<const std::byte> bytes_;
		EntityPageHeader header_;
	};

	// One EntityPage fills a shared-pool frame. Its bitmap is the liveness
	// authority; each slot stores a logical EntityId and durable generation so an
	// Entity Map entry cannot resolve another record after slot reuse.
	class EntityPage final {
	public:
		[[nodiscard]] static EntityPage create(const PageOwnerHeader &owner, EntityPageRecordKind recordKind);
		[[nodiscard]] static EntityPage open(const PageOwnerHeader &owner, std::span<const std::byte> bytes);

		EntityPage(const EntityPage &) = delete;
		EntityPage &operator=(const EntityPage &) = delete;
		EntityPage(EntityPage &&other) noexcept;
		EntityPage &operator=(EntityPage &&other) noexcept;

		[[nodiscard]] EntityPageHeader header() const;
		// Placement planners use this codec-derived value instead of duplicating a
		// capacity formula that depends on the durable page layout.
		[[nodiscard]] static uint16_t recordCapacity(EntityPageRecordKind recordKind);
		[[nodiscard]] EntityPage rebindOwner(const PageOwnerHeader &owner) const;
		[[nodiscard]] uint16_t insert(uint64_t entityId, const NodeEntityRecord &record);
		[[nodiscard]] uint16_t insert(uint64_t entityId, const EdgeEntityRecord &record);
		// Inserts no more than one fixed-page worth of Node or Edge records. It validates
		// every input before changing the page, then refreshes the durable page
		// checksums once for the complete page image.
		[[nodiscard]] std::vector<uint16_t> insertNodes(std::span<const IdentifiedNodeEntityRecord> records);
		[[nodiscard]] std::vector<uint16_t> insertEdges(std::span<const IdentifiedEdgeEntityRecord> records);
		[[nodiscard]] std::optional<NodeEntityRecord> findNode(uint16_t slot) const;
		[[nodiscard]] std::optional<NodeEntityRecord> findNode(uint16_t slot, uint32_t expectedSlotGeneration) const;
		[[nodiscard]] std::optional<NodeEntityRecord> findNode(uint16_t slot, uint32_t expectedSlotGeneration,
															uint64_t expectedEntityId) const;
		[[nodiscard]] std::optional<EdgeEntityRecord> findEdge(uint16_t slot) const;
		[[nodiscard]] std::optional<EdgeEntityRecord> findEdge(uint16_t slot, uint32_t expectedSlotGeneration) const;
		[[nodiscard]] std::optional<EdgeEntityRecord> findEdge(uint16_t slot, uint32_t expectedSlotGeneration,
															uint64_t expectedEntityId) const;
		[[nodiscard]] uint64_t entityId(uint16_t slot) const;
		[[nodiscard]] uint32_t slotGeneration(uint16_t slot) const;
		void replace(uint16_t slot, const NodeEntityRecord &record);
		void replace(uint16_t slot, uint32_t expectedSlotGeneration, const NodeEntityRecord &record);
		void replace(uint16_t slot, uint32_t expectedSlotGeneration, uint64_t expectedEntityId,
					 const NodeEntityRecord &record);
		void replace(uint16_t slot, const EdgeEntityRecord &record);
		void replace(uint16_t slot, uint32_t expectedSlotGeneration, const EdgeEntityRecord &record);
		void replace(uint16_t slot, uint32_t expectedSlotGeneration, uint64_t expectedEntityId,
					 const EdgeEntityRecord &record);
		void erase(uint16_t slot);
		void erase(uint16_t slot, uint32_t expectedSlotGeneration);
		void erase(uint16_t slot, uint32_t expectedSlotGeneration, uint64_t expectedEntityId);
		[[nodiscard]] std::vector<std::byte> serialize() const;

	private:
		template<typename IdentifiedRecord, typename Codec>
		[[nodiscard]] std::vector<uint16_t> insertRecords(std::span<const IdentifiedRecord> records, EntityPageRecordKind kind);

		explicit EntityPage(std::vector<std::byte> bytes, EntityPageHeader header);

		void validateLayout() const;
		void validateLiveSlot(uint16_t slot) const;
		void validateSlotGeneration(uint16_t slot, uint32_t expectedSlotGeneration) const;
		[[nodiscard]] size_t bitmapOffset() const;
		[[nodiscard]] size_t slotGenerationOffset(uint16_t slot) const;
		[[nodiscard]] size_t entityIdOffset(uint16_t slot) const;
		[[nodiscard]] size_t recordOffset(uint16_t slot) const;
		[[nodiscard]] bool isLive(uint16_t slot) const;
		void setLive(uint16_t slot, bool live);
		void setSlotGeneration(uint16_t slot, uint32_t generation);
		void setEntityId(uint16_t slot, uint64_t entityId);
		void refreshChecksums();

		std::vector<std::byte> bytes_;
		EntityPageHeader header_;
	};

} // namespace graph::storage
