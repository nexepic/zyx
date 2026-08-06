/**
 * @file Superblock.hpp
 * @brief Fixed bootstrap records for selecting a complete relocatable metadata generation.
 *
 * Superblocks are the only relocatable metadata stored before the physical-slot
 * region. They contain physical references only because resolving a root
 * directory through the Catalog would otherwise be recursive.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_SUPERBLOCK_MAGIC = 0x5358595AU; // Little-endian bytes: ZYXS
	// Version 30 removes retired Property and direct Vector-adjacency roots.
	// Earlier layouts are deliberately unsupported rather than silently
	// migrated or reinterpreted.
	constexpr uint16_t RSS_SUPERBLOCK_VERSION = 30;
	constexpr uint32_t RSS_SUPERBLOCK_BYTES = RSS_PAGE_SIZE;
	constexpr uint32_t RSS_SUPERBLOCK_HEADER_BYTES = 336;
	// Crash injection must leave this checksum untouched so a torn inactive
	// Superblock can never validate as a complete metadata generation.
	constexpr uint32_t RSS_SUPERBLOCK_HEADER_CRC_OFFSET = 116;
	// All lifecycle state is represented by the A/B Superblocks and their roots.
	// A nonzero flag is reserved for a future incompatible format version.
	constexpr uint32_t RSS_SUPERBLOCK_KNOWN_FLAGS = 0U;
	static_assert(RSS_FILE_BOOTSTRAP_BYTES >= 2U * RSS_SUPERBLOCK_BYTES);
	struct MetadataEmergencyReserveDescriptor {
		PhysicalSlotId physical_slot{};
		SegmentId pool_token{};
		Generation generation{};
		LogSequenceNumber committed_lsn{};

		[[nodiscard]] constexpr bool isAbsent() const {
			return !physical_slot.isValid() && !pool_token.isValid() && !generation.isValid() && !committed_lsn.isValid();
		}
		[[nodiscard]] constexpr bool isValid() const {
			return physical_slot.value() > 1U && pool_token.value() >= RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN &&
				   generation.isValid() && committed_lsn.isValid();
		}

		constexpr bool operator==(const MetadataEmergencyReserveDescriptor &) const = default;
	};

	struct Superblock {
		// Keep construction in one translation unit. This prevents an incremental
		// link from selecting an obsolete inline constructor after the layout grows.
		Superblock();

		DatabaseId database_id{};
		Generation generation{};
		LogSequenceNumber checkpoint_lsn{};
		uint32_t flags = 0;
		uint64_t data_region_offset = RSS_FILE_BOOTSTRAP_BYTES;
		PhysicalPageRef catalog_root{};
		PhysicalPageRef page_directory_root{};
		PhysicalPageRef extent_directory_root{};
		PhysicalPageRef allocator_root{};
		PhysicalPageRef node_entity_directory_root{};
		PhysicalPageRef edge_entity_directory_root{};
		// Stage 5 owns Property identity directly through (EntityKind, EntityId).
		PhysicalPageRef entity_property_directory_root{};
		PhysicalPageRef index_root_directory_root{};
		PhysicalPageRef vector_location_directory_root{};
		PhysicalPageRef vector_manifest_directory_root{};
		// Large values have their own logical namespace and an immutable manifest
		// directory. Data pages resolve through the ordinary Page Directory.
		PhysicalPageRef large_value_manifest_directory_root{};
		PhysicalPageRef state_directory_root{};
		// Retired slots remain durably queued until no active snapshot can reach
		// the generation that superseded them.
		PhysicalPageRef retire_queue_root{};
		// This regular metadata pool is bootstrap-addressed but intentionally
		// excluded from normal allocation. It supplies the last COW frames needed
		// to reclaim or publish state after normal pool growth hits ENOSPC.
		MetadataEmergencyReserveDescriptor metadata_emergency_reserve{};
		// Zero marks deliberate exhaustion. Every nonzero value is the next
		// unissued logical Page ID and is advanced in the same COW batch.
		uint64_t next_logical_page_id = 1;
		// Zero marks deliberate exhaustion. Newly created physical segments receive
		// a never-reused logical Segment ID from this durable high-water mark.
		uint64_t next_logical_segment_id = 1;
		// Node and Edge identity allocation is independent from physical fixed
		// segment placement. Zero marks deliberate exhaustion for each namespace.
		uint64_t next_logical_node_id = 1;
		uint64_t next_logical_edge_id = 1;
		// Vector IDs are globally unique and never reused, even when a vector is
		// replaced or its later graph publication fails after reservation.
		uint64_t next_logical_vector_id = 1;
		// Large values use globally unique, never-reused identities. A failed
		// transaction does not make an already reserved ID available again.
		uint64_t next_large_value_id = 1;
		// Zero marks deliberate exhaustion. Property keys use the persisted 32-bit
		// token representation; schema Token IDs retain a full 64-bit namespace.
		uint64_t next_property_key_token = 1;
		uint64_t next_schema_token_id = 1;
		constexpr bool operator==(const Superblock &) const = default;
	};

	class SuperblockCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_SUPERBLOCK_BYTES> encode(const Superblock &superblock);
		[[nodiscard]] static Superblock decode(std::span<const std::byte> bytes);

		// Selects the newest complete metadata generation from the A/B bootstrap pair.
		[[nodiscard]] static Superblock selectNewest(std::span<const std::byte> first,
													 std::span<const std::byte> second);
	};

} // namespace graph::storage
