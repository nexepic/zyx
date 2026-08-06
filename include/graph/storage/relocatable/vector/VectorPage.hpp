/**
 * @file VectorPage.hpp
 * @brief Checked fixed-record raw/PQ vector page codec for relocatable.
 *
 * A vector page owns one shared-pool frame and stores records with one fixed
 * byte stride. Unlike a Property slotted page, vectors use a compact bitmap so
 * a stable vector location is a Page ID plus slot ordinal and no per-record
 * offset needs to be persisted or repaired during page-local maintenance.
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

namespace graph::storage {

	constexpr uint32_t RSS_VECTOR_PAGE_MAGIC = 0x5650595AU; // Little-endian bytes: ZYPV
	constexpr uint16_t RSS_VECTOR_PAGE_VERSION = 3;
	constexpr uint16_t RSS_VECTOR_PAGE_HEADER_BYTES = 72;

	// Prefixes avoid Windows macro collisions.
	enum class VectorStorageKind : uint16_t {
		VSK_RAW = 1,
		VSK_PQ = 2,
	};

	struct VectorPageHeader {
		uint64_t index_id = 0;
		PageId page_id{};
		Generation generation{};
		VectorStorageKind kind = VectorStorageKind::VSK_RAW;
		uint32_t record_bytes = 0;
		uint16_t slot_capacity = 0;
		uint16_t live_count = 0;
		uint16_t bitmap_bytes = 0;
		LogSequenceNumber committed_lsn{};
		// Raw pages set this to zero. Every PQ page is bound to exactly one
		// immutable Codebook generation, preventing stale codes from being used
		// with a later codebook after a rebuild publication.
		Generation codebook_generation{};

		constexpr bool operator==(const VectorPageHeader &) const = default;
	};

	class VectorPageHeaderCodec final {
	public:
		using Bytes = std::array<std::byte, RSS_VECTOR_PAGE_HEADER_BYTES>;

		[[nodiscard]] static Bytes encode(const VectorPageHeader &header);
		[[nodiscard]] static VectorPageHeader decode(std::span<const std::byte> bytes);
	};

	// A durable per-slot generation table prevents a reused slot ordinal from
	// resolving a stale Vector Location. The page-local generation changes only
	// when an erased slot is reused, so overwrites preserve logical identity.
	class VectorPage final {
	public:
		[[nodiscard]] static VectorPage create(const PageOwnerHeader &owner, const VectorPageHeader &header);
		[[nodiscard]] static VectorPage open(const PageOwnerHeader &owner, std::span<const std::byte> bytes);
		VectorPage(const VectorPage &) = delete;
		VectorPage &operator=(const VectorPage &) = delete;
		VectorPage(VectorPage &&other) noexcept;
		VectorPage &operator=(VectorPage &&other) noexcept;

		[[nodiscard]] VectorPageHeader header() const;
		// Rebinds only the physical owner generation and publication LSN. The
		// bitmap, fixed record bytes, and per-slot generations stay byte-for-byte
		// stable so Vector Location Directory references remain valid.
		[[nodiscard]] VectorPage rebindOwner(const PageOwnerHeader &owner) const;
		[[nodiscard]] uint16_t insert(std::span<const std::byte> record);
		[[nodiscard]] std::vector<std::byte> read(uint16_t slot) const;
		[[nodiscard]] std::vector<std::byte> read(uint16_t slot, uint32_t expectedSlotGeneration) const;
		[[nodiscard]] uint32_t slotGeneration(uint16_t slot) const;
		// Derived PQ mappings resolve a page first, then prove the Vector ID by
		// scanning that one fixed-capacity page. The record prefix is little-endian
		// uint64_t Vector ID, as written by VectorPageStore::materializePq.
		[[nodiscard]] std::optional<uint16_t> findVectorId(uint64_t vectorId) const;
		void overwrite(uint16_t slot, std::span<const std::byte> record);
		void overwrite(uint16_t slot, uint32_t expectedSlotGeneration, std::span<const std::byte> record);
		void erase(uint16_t slot);
		void erase(uint16_t slot, uint32_t expectedSlotGeneration);
		[[nodiscard]] std::vector<std::byte> serialize() const;

	private:
		explicit VectorPage(std::vector<std::byte> bytes, VectorPageHeader header);
		void validateLayout() const;
		void validateSlot(uint16_t slot) const;
		void validateSlotGeneration(uint16_t slot, uint32_t expectedSlotGeneration) const;
		[[nodiscard]] size_t bitmapOffset() const;
		[[nodiscard]] size_t slotGenerationOffset(uint16_t slot) const;
		[[nodiscard]] size_t recordOffset(uint16_t slot) const;
		[[nodiscard]] bool isLive(uint16_t slot) const;
		void setLive(uint16_t slot, bool live);
		void setSlotGeneration(uint16_t slot, uint32_t generation);
		void refreshChecksums();

		std::vector<std::byte> bytes_;
		VectorPageHeader header_;
	};

} // namespace graph::storage
