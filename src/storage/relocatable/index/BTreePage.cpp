/**
 * @file BTreePage.cpp
 * @brief Explicit B+Tree record codecs for relocatable slotted index pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/BTreePage.hpp"
#include "graph/debug/PerfTrace.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t KIND_OFFSET = 8;
		constexpr size_t LEVEL_OFFSET = 10;
		constexpr size_t FLAGS_OFFSET = 12;
		constexpr size_t INDEX_ID_OFFSET = 16;
		constexpr size_t PAGE_ID_OFFSET = 24;
		constexpr size_t GENERATION_OFFSET = 32;
		constexpr size_t LEFT_SIBLING_OFFSET = 40;
		constexpr size_t RIGHT_SIBLING_OFFSET = 48;
		constexpr size_t LEFTMOST_CHILD_OFFSET = 56;
		constexpr size_t ENTRY_COUNT_OFFSET = 64;
		constexpr size_t PAGE_RESERVED_OFFSET = 68;

		constexpr size_t ENTRY_KEY_BYTES_OFFSET = 8;
		constexpr size_t ENTRY_FLAGS_OFFSET = 12;
		constexpr size_t ENTRY_VALUE_OFFSET = 16;
		constexpr size_t ENTRY_RESERVED_OFFSET = 24;

		constexpr size_t PACKED_VERSION_OFFSET = 4;
		constexpr size_t PACKED_HEADER_BYTES_OFFSET = 6;
		constexpr size_t PACKED_RESTART_INTERVAL_OFFSET = 8;
		constexpr size_t PACKED_FLAGS_OFFSET = 10;
		constexpr size_t PACKED_ENTRY_COUNT_OFFSET = 12;
		constexpr size_t PACKED_RESTART_COUNT_OFFSET = 16;
		constexpr size_t PACKED_STREAM_BYTES_OFFSET = 20;
		constexpr size_t PACKED_RESERVED0_OFFSET = 24;
		constexpr size_t PACKED_RESERVED1_OFFSET = 28;

		[[noreturn]] void throwBTreePageError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isNodeKind(BTreeNodeKind kind) {
			return kind == BTreeNodeKind::BPN_LEAF || kind == BTreeNodeKind::BPN_INTERNAL;
		}

		[[nodiscard]] bool isLeafValueLayout(BTreeLeafValueLayout layout) {
			return layout == BTreeLeafValueLayout::BLVL_U64 || layout == BTreeLeafValueLayout::BLVL_EMPTY;
		}

		void validateValueLayout(BTreeNodeKind kind, BTreeLeafValueLayout layout) {
			const uint8_t invalid = static_cast<uint8_t>(!isNodeKind(kind)) |
									static_cast<uint8_t>(!isLeafValueLayout(layout)) |
									static_cast<uint8_t>(kind == BTreeNodeKind::BPN_INTERNAL &&
														 layout != BTreeLeafValueLayout::BLVL_U64);
			if (invalid != 0U) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable B+Tree page has an invalid leaf-value layout");
			}
		}

		[[nodiscard]] bool storesValue(BTreeNodeKind kind, BTreeLeafValueLayout layout) {
			validateValueLayout(kind, layout);
			return kind == BTreeNodeKind::BPN_INTERNAL || layout == BTreeLeafValueLayout::BLVL_U64;
		}

		void validateHeader(const BTreePageHeader &header) {
			const uint8_t invalid =
					static_cast<uint8_t>(header.index_id == 0U) | static_cast<uint8_t>(!header.page_id.isValid()) |
					static_cast<uint8_t>(!header.generation.isValid()) |
					static_cast<uint8_t>(!isNodeKind(header.kind)) |
					static_cast<uint8_t>(!isLeafValueLayout(header.leaf_value_layout)) |
					static_cast<uint8_t>(header.kind == BTreeNodeKind::BPN_LEAF && header.level != 0U) |
					static_cast<uint8_t>(header.kind == BTreeNodeKind::BPN_LEAF && header.leftmost_child.isValid()) |
					static_cast<uint8_t>(header.kind == BTreeNodeKind::BPN_INTERNAL && header.level == 0U) |
					static_cast<uint8_t>(header.kind == BTreeNodeKind::BPN_INTERNAL &&
										 !header.leftmost_child.isValid()) |
					static_cast<uint8_t>(header.kind == BTreeNodeKind::BPN_INTERNAL &&
										 header.leaf_value_layout != BTreeLeafValueLayout::BLVL_U64);
			if (invalid != 0U) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable B+Tree page header has invalid identity, node kind, or child layout");
			}
		}

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] bool sameKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::ranges::equal(left, right);
		}

		template<class ByteVector>
		void appendVarint(ByteVector &bytes, uint64_t value) {
			while (value >= 0x80U) {
				bytes.push_back(static_cast<std::byte>((value & 0x7FU) | 0x80U));
				value >>= 7U;
			}
			bytes.push_back(static_cast<std::byte>(value));
		}

		[[nodiscard]] uint64_t readVarint(std::span<const std::byte> bytes, size_t &offset) {
			uint64_t value = 0U;
			for (uint32_t shift = 0U; shift < 64U; shift += 7U) {
				if (offset >= bytes.size()) {
					throwBTreePageError(FormatError::FERR_BUFFER_TOO_SMALL,
										"relocatable packed B+Tree varint is truncated");
				}
				const uint8_t raw = std::to_integer<uint8_t>(bytes[offset++]);
				if (shift == 63U && (raw & 0x7EU) != 0U) {
					throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										"relocatable packed B+Tree varint overflows 64 bits");
				}
				value |= static_cast<uint64_t>(raw & 0x7FU) << shift;
				if ((raw & 0x80U) == 0U) {
					return value;
				}
			}
			throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable packed B+Tree varint is too long");
		}

		[[nodiscard]] size_t commonPrefix(std::span<const std::byte> left, std::span<const std::byte> right) {
			size_t count = 0U;
			while (count < left.size() && count < right.size() && left[count] == right[count]) { // 				// encoder callers first require left < right, so left cannot extend right after a shared prefix.
				++count;
			}
			return count;
		}

		struct PackedHeader {
			uint16_t restart_interval = RSS_BTREE_PACKED_RESTART_INTERVAL;
			uint32_t entry_count = 0U;
			uint32_t restart_count = 0U;
			uint32_t stream_bytes = 0U;
			size_t stream_offset = RSS_BTREE_PACKED_HEADER_BYTES;
		};

		[[nodiscard]] PackedHeader decodePackedHeader(std::span<const std::byte> bytes) {
			if (bytes.size() < RSS_BTREE_PACKED_HEADER_BYTES) {
				throwBTreePageError(FormatError::FERR_BUFFER_TOO_SMALL,
									"relocatable packed B+Tree payload header is truncated");
			}
			if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_BTREE_PACKED_MAGIC) {
				throwBTreePageError(FormatError::FERR_INVALID_MAGIC,
									"relocatable packed B+Tree payload has an invalid magic");
			}
			if (LittleEndianCodec::readU16(bytes, PACKED_VERSION_OFFSET) != RSS_BTREE_PACKED_VERSION) {
				throwBTreePageError(FormatError::FERR_UNSUPPORTED_VERSION,
									"relocatable packed B+Tree payload has an unsupported version");
			}
			if (LittleEndianCodec::readU16(bytes, PACKED_HEADER_BYTES_OFFSET) != RSS_BTREE_PACKED_HEADER_BYTES) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree payload has an invalid header size");
			}
			const uint8_t invalidReserved =
					static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, PACKED_FLAGS_OFFSET) != 0U) |
					static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, PACKED_RESERVED0_OFFSET) != 0U) |
					static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, PACKED_RESERVED1_OFFSET) != 0U);
			if (invalidReserved != 0U) {
				throwBTreePageError(FormatError::FERR_RESERVED_BITS,
									"relocatable packed B+Tree payload has invalid fixed fields");
			}
			PackedHeader header;
			header.restart_interval = LittleEndianCodec::readU16(bytes, PACKED_RESTART_INTERVAL_OFFSET);
			header.entry_count = LittleEndianCodec::readU32(bytes, PACKED_ENTRY_COUNT_OFFSET);
			header.restart_count = LittleEndianCodec::readU32(bytes, PACKED_RESTART_COUNT_OFFSET);
			header.stream_bytes = LittleEndianCodec::readU32(bytes, PACKED_STREAM_BYTES_OFFSET);
			const uint64_t expectedRestartCount =
					header.entry_count == 0U
							? 0U
							: (static_cast<uint64_t>(header.entry_count) + RSS_BTREE_PACKED_RESTART_INTERVAL - 1U) /
									  RSS_BTREE_PACKED_RESTART_INTERVAL;
			if (header.restart_interval == 0U || header.restart_interval != RSS_BTREE_PACKED_RESTART_INTERVAL ||
				header.restart_count != expectedRestartCount) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree payload has inconsistent restart metadata");
			}
			const uint64_t streamOffset = static_cast<uint64_t>(RSS_BTREE_PACKED_HEADER_BYTES) +
										  static_cast<uint64_t>(header.restart_count) * sizeof(uint32_t);
			if (streamOffset > bytes.size() || header.stream_bytes != bytes.size() - streamOffset) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree payload length disagrees with its restart table");
			}
			header.stream_offset = static_cast<size_t>(streamOffset);
			return header;
		}

		[[nodiscard]] utils::OwnedStdVector<uint32_t> restartOffsets(std::span<const std::byte> bytes,
														   const PackedHeader &header) {
			utils::OwnedStdVector<uint32_t> offsets;
			offsets.reserve(header.restart_count);
			for (uint32_t index = 0U; index < header.restart_count; ++index) {
				const size_t offset = RSS_BTREE_PACKED_HEADER_BYTES + static_cast<size_t>(index) * sizeof(uint32_t);
				const uint32_t restart = LittleEndianCodec::readU32(bytes, offset);
				if (restart >= header.stream_bytes || (index != 0U && restart <= offsets.back())) {
					throwBTreePageError(FormatError::FERR_INVALID_HEADER,
										"relocatable packed B+Tree restart offsets are not strictly ordered");
				}
				offsets.push_back(restart);
			}
			if (header.entry_count == 0U) {
				if (header.restart_count != 0U || header.stream_bytes != 0U) {
					throwBTreePageError(FormatError::FERR_INVALID_HEADER,
										"relocatable empty packed B+Tree payload has restart data");
				}
			} else if (offsets.empty() || offsets.front() != 0U) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree payload does not start at a restart point");
			}
			return offsets;
		}

		[[nodiscard]] std::optional<BTreePageEntry> decodeOne(std::span<const std::byte> stream, size_t &offset,
															  utils::OwnedStdVector<std::byte> &previous, bool restart,
															  bool includesValue) {
			if (offset == stream.size()) {
				return std::nullopt;
			}
			const uint64_t shared = readVarint(stream, offset);
			const uint64_t suffixBytes = readVarint(stream, offset);
			if (restart && shared != 0U) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree restart entry has a nonzero prefix");
			}
			if (shared > previous.size() || suffixBytes > stream.size() - offset ||
				suffixBytes > std::numeric_limits<size_t>::max() - static_cast<size_t>(shared)) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree key lengths exceed the stream");
			}
			// Reserve the complete reconstructed key once. Growing a shared-prefix
			// vector during suffix insertion otherwise retains nearly double capacity.
			utils::OwnedStdVector<std::byte> key;
            key.resize(static_cast<size_t>(shared + suffixBytes));
            std::copy_n(previous.begin(), static_cast<size_t>(shared), key.begin());
            std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(offset), static_cast<size_t>(suffixBytes),
                key.begin() + static_cast<std::ptrdiff_t>(shared));
			offset += static_cast<size_t>(suffixBytes);
			const uint64_t value = includesValue ? readVarint(stream, offset) : 0U;
			previous = key;
			return BTreePageEntry{std::move(key), value};
		}

	} // namespace

	BTreePageHeaderCodec::Bytes BTreePageHeaderCodec::encode(const BTreePageHeader &header) {
		validateHeader(header);
		Bytes bytes{};
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_BTREE_PAGE_HEADER_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_BTREE_PAGE_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_BTREE_PAGE_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, KIND_OFFSET, static_cast<uint16_t>(header.kind));
		LittleEndianCodec::writeU16(bytes, LEVEL_OFFSET, header.level);
		LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, static_cast<uint16_t>(header.leaf_value_layout));
		LittleEndianCodec::writeU64(bytes, INDEX_ID_OFFSET, header.index_id);
		LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, header.page_id.value());
		LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, header.generation.value());
		LittleEndianCodec::writeU64(bytes, LEFT_SIBLING_OFFSET, header.left_sibling.value());
		LittleEndianCodec::writeU64(bytes, RIGHT_SIBLING_OFFSET, header.right_sibling.value());
		LittleEndianCodec::writeU64(bytes, LEFTMOST_CHILD_OFFSET, header.leftmost_child.value());
		LittleEndianCodec::writeU32(bytes, ENTRY_COUNT_OFFSET, header.entry_count);
		return bytes;
	}

	BTreePageHeader BTreePageHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_BTREE_PAGE_HEADER_BYTES) {
			throwBTreePageError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable B+Tree page header is truncated");
		}
		if (bytes.size() != RSS_BTREE_PAGE_HEADER_BYTES) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable B+Tree page header has an invalid length");
		}
		if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_BTREE_PAGE_HEADER_MAGIC) {
			throwBTreePageError(FormatError::FERR_INVALID_MAGIC, "relocatable B+Tree page header has an invalid magic");
		}
		if (LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_BTREE_PAGE_VERSION) {
			throwBTreePageError(FormatError::FERR_UNSUPPORTED_VERSION,
								"relocatable B+Tree page header has an unsupported version");
		}
		if (LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_BTREE_PAGE_HEADER_BYTES) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable B+Tree page header has an invalid header size");
		}
		const uint32_t encodedLeafValueLayout = LittleEndianCodec::readU32(bytes, FLAGS_OFFSET);
		if (encodedLeafValueLayout > static_cast<uint32_t>(BTreeLeafValueLayout::BLVL_EMPTY) ||
			LittleEndianCodec::readU32(bytes, PAGE_RESERVED_OFFSET) != 0U) {
			throwBTreePageError(FormatError::FERR_RESERVED_BITS,
								"relocatable B+Tree page header has invalid fixed or reserved fields");
		}
		BTreePageHeader header;
		header.kind = static_cast<BTreeNodeKind>(LittleEndianCodec::readU16(bytes, KIND_OFFSET));
		header.level = LittleEndianCodec::readU16(bytes, LEVEL_OFFSET);
		header.leaf_value_layout = static_cast<BTreeLeafValueLayout>(encodedLeafValueLayout);
		header.index_id = LittleEndianCodec::readU64(bytes, INDEX_ID_OFFSET);
		header.page_id = PageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		header.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		header.left_sibling = PageId(LittleEndianCodec::readU64(bytes, LEFT_SIBLING_OFFSET));
		header.right_sibling = PageId(LittleEndianCodec::readU64(bytes, RIGHT_SIBLING_OFFSET));
		header.leftmost_child = PageId(LittleEndianCodec::readU64(bytes, LEFTMOST_CHILD_OFFSET));
		header.entry_count = LittleEndianCodec::readU32(bytes, ENTRY_COUNT_OFFSET);
		validateHeader(header);
		return header;
	}

	size_t BTreePageEntryCodec::encodedSize(const BTreePageEntry &entry) {
		if (entry.key.size() > std::numeric_limits<uint32_t>::max()) {
			throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable B+Tree entry key length exceeds the persisted format");
		}
		if (entry.key.size() > std::numeric_limits<size_t>::max() - RSS_BTREE_ENTRY_HEADER_BYTES) {
			throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable B+Tree entry encoded size overflows");
		}
		return RSS_BTREE_ENTRY_HEADER_BYTES + entry.key.size();
	}

	std::vector<std::byte> BTreePageEntryCodec::encode(const BTreePageEntry &entry) {
		std::vector<std::byte> bytes(encodedSize(entry));
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_BTREE_ENTRY_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_BTREE_ENTRY_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_BTREE_ENTRY_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, ENTRY_KEY_BYTES_OFFSET, static_cast<uint32_t>(entry.key.size()));
		LittleEndianCodec::writeU64(bytes, ENTRY_VALUE_OFFSET, entry.value_or_child);
		if (!entry.key.empty()) {
			std::copy(entry.key.begin(), entry.key.end(), bytes.begin() + RSS_BTREE_ENTRY_HEADER_BYTES);
		}
		return bytes;
	}

	BTreePageEntry BTreePageEntryCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_BTREE_ENTRY_HEADER_BYTES) {
			throwBTreePageError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable B+Tree entry is truncated");
		}
		const uint8_t invalid =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_BTREE_ENTRY_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_BTREE_ENTRY_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_BTREE_ENTRY_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, ENTRY_FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, ENTRY_RESERVED_OFFSET) != 0U);
		if (invalid != 0U) {
			throwBTreePageError(FormatError::FERR_RESERVED_BITS,
								"relocatable B+Tree entry has invalid fixed or reserved fields");
		}
		const uint32_t keyBytes = LittleEndianCodec::readU32(bytes, ENTRY_KEY_BYTES_OFFSET);
		if (keyBytes != bytes.size() - RSS_BTREE_ENTRY_HEADER_BYTES) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable B+Tree entry length disagrees with its encoded key");
		}
		return {utils::OwnedStdVector<std::byte>(bytes.begin() + RSS_BTREE_ENTRY_HEADER_BYTES, bytes.end()),
				LittleEndianCodec::readU64(bytes, ENTRY_VALUE_OFFSET)};
	}

	size_t PackedBTreePageCodec::encodedSize(BTreeNodeKind kind, std::span<const BTreePageEntry> entries,
			BTreeLeafValueLayout leafValueLayout) {
		validateValueLayout(kind, leafValueLayout);
		const bool includesValue = storesValue(kind, leafValueLayout);
		if (entries.size() > std::numeric_limits<uint32_t>::max()) {
			throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW, "packed B+Tree entry count overflows");
		}
		const auto varintBytes = [](uint64_t value) {
			size_t bytes = 1;
			while (value >= 0x80U) { value >>= 7U; ++bytes; }
			return bytes;
		};
		const uint64_t restarts = (static_cast<uint64_t>(entries.size()) + RSS_BTREE_PACKED_RESTART_INTERVAL - 1U) /
			RSS_BTREE_PACKED_RESTART_INTERVAL;
		uint64_t total = RSS_BTREE_PACKED_HEADER_BYTES + restarts * sizeof(uint32_t);
		for (size_t index = 0; index < entries.size(); ++index) {
			const auto &entry = entries[index];
			if (index && !lessKey(entries[index - 1].key, entry.key)) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER, "packed B+Tree keys are not strictly ordered");
			}
			if ((kind == BTreeNodeKind::BPN_INTERNAL && !PageId(entry.value_or_child).isValid()) ||
				(!includesValue && entry.value_or_child != 0U)) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER, "packed B+Tree entry value is invalid");
			}
			const size_t shared = index % RSS_BTREE_PACKED_RESTART_INTERVAL == 0U ? 0U :
				commonPrefix(entries[index - 1].key, entry.key);
			const size_t suffix = entry.key.size() - shared;
			// Bound before adding a potentially enormous key size.
			if (suffix > std::numeric_limits<uint32_t>::max()) {
				throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW, "packed B+Tree payload is too large");
			}
			total += varintBytes(shared) + varintBytes(suffix) + suffix +
				(includesValue ? varintBytes(entry.value_or_child) : 0U);
			if (total > std::numeric_limits<uint32_t>::max()) {
				throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW, "packed B+Tree payload is too large");
			}
		}
		return static_cast<size_t>(total);
	}

	PackedBTreePageCodec::BoundarySizes PackedBTreePageCodec::encodedBoundarySizes(
		BTreeNodeKind kind, std::span<const BTreePageEntry> entries, BTreeLeafValueLayout leafValueLayout) {
		// Authenticate order, values and layout before deriving any boundary.
		static_cast<void>(encodedSize(kind, entries, leafValueLayout));
		BoundarySizes result{utils::OwnedStdVector<size_t>(entries.size() + 1U, RSS_BTREE_PACKED_HEADER_BYTES),
			utils::OwnedStdVector<size_t>(entries.size() + 1U, RSS_BTREE_PACKED_HEADER_BYTES)};
		const bool includesValue = storesValue(kind, leafValueLayout);
		const auto varintBytes = [](uint64_t value) {
			uint64_t bytes = 1U;
			while (value >= 0x80U) { value >>= 7U; ++bytes; }
			return bytes;
		};
		const auto entryBytes = [&](size_t i, bool restart) {
			const auto &entry = entries[i];
			const size_t shared = restart || i == 0U ? 0U : commonPrefix(entries[i - 1U].key, entry.key);
			const size_t suffix = entry.key.size() - shared;
			return varintBytes(shared) + varintBytes(suffix) + suffix +
				(includesValue ? varintBytes(entry.value_or_child) : 0U);
		};
		const auto payloadBytes = [](uint64_t stream, size_t count) {
			const uint64_t restarts = (static_cast<uint64_t>(count) + RSS_BTREE_PACKED_RESTART_INTERVAL - 1U) /
				RSS_BTREE_PACKED_RESTART_INTERVAL;
			const uint64_t bytes = RSS_BTREE_PACKED_HEADER_BYTES + restarts * sizeof(uint32_t) + stream;
			if (bytes > std::numeric_limits<uint32_t>::max()) {
				throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW, "packed B+Tree boundary payload is too large");
			}
			return static_cast<size_t>(bytes);
		};
		uint64_t prefixStream = 0U;
		for (size_t i = 0U; i < entries.size(); ++i) {
			prefixStream += entryBytes(i, i % RSS_BTREE_PACKED_RESTART_INTERVAL == 0U);
			result.prefixes[i + 1U] = payloadBytes(prefixStream, i + 1U);
		}
		// A suffix starting at i restarts at i, i+R, ... . Track normal and
		// restart costs by residue so every candidate is exact in linear work.
		std::array<uint64_t, RSS_BTREE_PACKED_RESTART_INTERVAL> normalByResidue{}, restartByResidue{};
		uint64_t normalSuffix = 0U;
		for (size_t i = entries.size(); i-- > 0U;) {
			const size_t residue = i % RSS_BTREE_PACKED_RESTART_INTERVAL;
			const uint64_t normal = entryBytes(i, false);
			normalSuffix += normal;
			normalByResidue[residue] += normal;
			restartByResidue[residue] += entryBytes(i, true);
			result.suffixes[i] = payloadBytes(normalSuffix - normalByResidue[residue] + restartByResidue[residue],
				entries.size() - i);
		}
		return result;
	}

	template<class ByteVector>
	static ByteVector encodePackedEntries(BTreeNodeKind kind, std::span<const BTreePageEntry> entries,
														BTreeLeafValueLayout leafValueLayout) {
		validateValueLayout(kind, leafValueLayout);
		const bool includesValue = storesValue(kind, leafValueLayout);
		if (entries.size() > std::numeric_limits<uint32_t>::max()) {
			throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable packed B+Tree entry count overflows the payload");
		}
		const uint32_t restartCount =
				entries.empty() ? 0U
								: static_cast<uint32_t>((entries.size() + RSS_BTREE_PACKED_RESTART_INTERVAL - 1U) /
														RSS_BTREE_PACKED_RESTART_INTERVAL);
		ByteVector stream;
		utils::OwnedStdVector<uint32_t> offsets;
		offsets.reserve(restartCount);
		utils::OwnedStdVector<std::byte> previous;
		for (size_t index = 0U; index < entries.size(); ++index) {
			const BTreePageEntry &entry = entries[index];
			if (index != 0U && !lessKey(entries[index - 1U].key, entry.key)) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree keys are not strictly ordered");
			}
			if (kind == BTreeNodeKind::BPN_INTERNAL && !PageId(entry.value_or_child).isValid()) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed internal B+Tree entry requires a valid child Page ID");
			}
			if (!includesValue && entry.value_or_child != 0U) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable key-only B+Tree leaf entry has a nonempty value");
			}
			const bool restart = (index % RSS_BTREE_PACKED_RESTART_INTERVAL) == 0U;
			if (restart) {
				if (stream.size() > std::numeric_limits<uint32_t>::max()) {
					throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										"relocatable packed B+Tree stream offset overflows the payload");
				}
				offsets.push_back(static_cast<uint32_t>(stream.size()));
			}
			const size_t shared = restart ? 0U : commonPrefix(previous, entry.key);
			appendVarint(stream, shared);
			appendVarint(stream, entry.key.size() - shared);
			stream.insert(stream.end(), entry.key.begin() + static_cast<std::ptrdiff_t>(shared), entry.key.end());
			if (includesValue) {
				appendVarint(stream, entry.value_or_child);
			}
			previous = entry.key;
		}
		const uint64_t totalBytes = static_cast<uint64_t>(RSS_BTREE_PACKED_HEADER_BYTES) +
									static_cast<uint64_t>(offsets.size()) * sizeof(uint32_t) + stream.size();
		if (totalBytes > std::numeric_limits<uint32_t>::max()) {
			throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable packed B+Tree payload is too large");
		}
		ByteVector bytes(static_cast<size_t>(totalBytes));
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_BTREE_PACKED_MAGIC);
		LittleEndianCodec::writeU16(bytes, PACKED_VERSION_OFFSET, RSS_BTREE_PACKED_VERSION);
		LittleEndianCodec::writeU16(bytes, PACKED_HEADER_BYTES_OFFSET, RSS_BTREE_PACKED_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, PACKED_RESTART_INTERVAL_OFFSET, RSS_BTREE_PACKED_RESTART_INTERVAL);
		LittleEndianCodec::writeU32(bytes, PACKED_ENTRY_COUNT_OFFSET, static_cast<uint32_t>(entries.size()));
		LittleEndianCodec::writeU32(bytes, PACKED_RESTART_COUNT_OFFSET, static_cast<uint32_t>(offsets.size()));
		LittleEndianCodec::writeU32(bytes, PACKED_STREAM_BYTES_OFFSET, static_cast<uint32_t>(stream.size()));
		for (size_t index = 0U; index < offsets.size(); ++index) {
			LittleEndianCodec::writeU32(bytes, RSS_BTREE_PACKED_HEADER_BYTES + index * sizeof(uint32_t),
										offsets[index]);
		}
		std::copy(stream.begin(), stream.end(),
				  bytes.begin() + RSS_BTREE_PACKED_HEADER_BYTES + offsets.size() * sizeof(uint32_t));
		return bytes;
	}

 std::vector<std::byte> PackedBTreePageCodec::encode(BTreeNodeKind kind,
   std::span<const BTreePageEntry> entries, BTreeLeafValueLayout layout) {
  return encodePackedEntries<std::vector<std::byte>>(kind, entries, layout);
 }
 utils::OwnedStdVector<std::byte> PackedBTreePageCodec::encodeOwned(BTreeNodeKind kind,
   std::span<const BTreePageEntry> entries, BTreeLeafValueLayout layout) {
  return encodePackedEntries<utils::OwnedStdVector<std::byte>>(kind, entries, layout);
 }

	utils::OwnedStdVector<BTreePageEntry> PackedBTreePageCodec::decode(BTreeNodeKind kind, std::span<const std::byte> bytes,
															 BTreeLeafValueLayout leafValueLayout) {
		debug::PerfTrace::addValue("native_btree.read.packed_page_decodes", 1);
		validateValueLayout(kind, leafValueLayout);
		const bool includesValue = storesValue(kind, leafValueLayout);
		const PackedHeader header = decodePackedHeader(bytes);
		const utils::OwnedStdVector<uint32_t> offsets = restartOffsets(bytes, header);
		utils::OwnedStdVector<BTreePageEntry> entries;
		entries.reserve(header.entry_count);
		utils::OwnedStdVector<std::byte> previous;
		const std::span<const std::byte> stream = bytes.subspan(header.stream_offset, header.stream_bytes);
		size_t offset = 0U;
		for (uint32_t index = 0U; index < header.entry_count; ++index) {
			const bool restart = (index % header.restart_interval) == 0U;
			if (restart && offset != offsets[index / header.restart_interval]) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree restart offset does not point to an entry");
			}
			auto entry = decodeOne(stream, offset, previous, restart, includesValue);
			if (!entry.has_value()) {
				throwBTreePageError(FormatError::FERR_BUFFER_TOO_SMALL,
									"relocatable packed B+Tree entry stream ended early");
			}
			if (!entries.empty() && !lessKey(entries.back().key, entry->key)) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree entries are not strictly ordered");
			}
			if (kind == BTreeNodeKind::BPN_INTERNAL && !PageId(entry->value_or_child).isValid()) {
				throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed internal B+Tree entry has an invalid child Page ID");
			}
			entries.push_back(std::move(*entry));
		}
		if (offset != stream.size()) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable packed B+Tree entry stream has trailing bytes");
		}
		return entries;
	}

	uint32_t PackedBTreePageCodec::entryCount(std::span<const std::byte> bytes) {
		const PackedHeader header = decodePackedHeader(bytes);
		static_cast<void>(restartOffsets(bytes, header));
		return header.entry_count;
	}

	std::optional<BTreePageEntry> PackedBTreePageCodec::find(BTreeNodeKind kind, std::span<const std::byte> bytes,
															 std::span<const std::byte> key,
															 BTreeLeafValueLayout leafValueLayout) {
		validateValueLayout(kind, leafValueLayout);
		const bool includesValue = storesValue(kind, leafValueLayout);
		const PackedHeader header = decodePackedHeader(bytes);
		const utils::OwnedStdVector<uint32_t> offsets = restartOffsets(bytes, header);
		if (header.entry_count == 0U) {
			return std::nullopt;
		}
		const std::span<const std::byte> stream = bytes.subspan(header.stream_offset, header.stream_bytes);
		// Binary-search restart keys, then decode only the selected restart group.
		const auto restartKeyAt = [&stream, &offsets, includesValue](size_t candidate) {
			size_t offset = offsets[candidate];
			utils::OwnedStdVector<std::byte> previous;
			const auto entry = decodeOne(stream, offset, previous, true, includesValue);
			// restartOffsets() proved this offset lies inside the stream; decodeOne()
			// consequently either returns the restart entry or throws a codec error.
			return entry->key;
		};
		size_t firstGreater = 0U;
		size_t upper = offsets.size();
		while (firstGreater < upper) {
			const size_t middle = firstGreater + (upper - firstGreater) / 2U;
			if (lessKey(key, restartKeyAt(middle))) {
				upper = middle;
			} else {
				firstGreater = middle + 1U;
			}
		}
		const size_t group = firstGreater == 0U ? 0U : firstGreater - 1U;
		size_t offset = offsets[group];
		utils::OwnedStdVector<std::byte> previous;
		const size_t first = group * header.restart_interval;
		for (size_t index = first; index < std::min<size_t>(header.entry_count, first + header.restart_interval);
			 ++index) {
			const auto entry = decodeOne(stream, offset, previous, index == first, includesValue);
			if (!entry.has_value()) {
				throwBTreePageError(FormatError::FERR_BUFFER_TOO_SMALL,
									"relocatable packed B+Tree entry stream ended during lookup");
			}
			if (sameKey(entry->key, key)) {
				if (kind == BTreeNodeKind::BPN_INTERNAL && !PageId(entry->value_or_child).isValid()) {
					throwBTreePageError(FormatError::FERR_INVALID_HEADER,
										"relocatable packed internal B+Tree entry has an invalid child Page ID");
				}
				return entry;
			}
			if (lessKey(key, entry->key)) {
				break;
			}
		}
		return std::nullopt;
	}

	BTreePage::BTreePage(SlottedPage page) : page_(std::move(page)) {}

	BTreePage::BTreePage(BTreePage &&other) noexcept : page_(std::move(other.page_)) {}

	BTreePage &BTreePage::operator=(BTreePage &&other) noexcept {
		if (this != &other) {
			page_ = std::move(other.page_);
		}
		return *this;
	}

	void BTreePage::validateNewPage(const PageOwnerHeader &owner, const BTreePageHeader &header) {
		const uint8_t invalidOwner = static_cast<uint8_t>(owner.kind != PageOwnerKind::POK_INDEX) |
									 static_cast<uint8_t>(owner.owner_id != header.index_id) |
									 static_cast<uint8_t>(owner.page_id != header.page_id) |
									 static_cast<uint8_t>(owner.generation != header.generation);
		if (invalidOwner != 0U) {
			throwBTreePageError(FormatError::FERR_INVALID_STATE,
								"relocatable B+Tree page header does not match its index-page owner");
		}
		if (header.entry_count != 0U) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable new B+Tree page cannot declare entries before they are materialized");
		}
		SlottedPage::validateOwner(owner);
		validateHeader(header);
	}

	BTreePage BTreePage::create(const PageOwnerHeader &owner, const BTreePageHeader &header) {
		validateNewPage(owner, header);
		SlottedPage page = SlottedPage::create(owner);
		static_cast<void>(page.insert(BTreePageHeaderCodec::encode(header)));
		const utils::OwnedStdVector<BTreePageEntry> emptyEntries;
		static_cast<void>(
				page.insert(PackedBTreePageCodec::encode(header.kind, emptyEntries, header.leaf_value_layout)));
		return BTreePage(std::move(page));
	}

	BTreePage BTreePage::createWithEntries(const PageOwnerHeader &owner, BTreePageHeader header,
										   std::span<const BTreePageEntry> entries) {
		if (entries.size() > std::numeric_limits<uint32_t>::max()) {
			throwBTreePageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable B+Tree page entry count cannot fit in its persisted header");
		}
		if (header.entry_count != 0U) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable bulk B+Tree page must start with an empty header entry count");
		}
		BTreePage page = create(owner, header);
		page.writePackedEntries(entries);
		header.entry_count = static_cast<uint32_t>(entries.size());
		page.writeHeader(header);
		page.validateLayout();
		return page;
	}

	BTreePage BTreePage::open(const PageOwnerHeader &owner, std::span<const std::byte> bytes) {
		BTreePage page(SlottedPage::open(owner, bytes));
		page.validateLayout();
		return page;
	}

    BTreePage BTreePage::open(SlottedPage source) {
        BTreePage page(std::move(source));
        page.validateLayout();
        return page;
    }

    BTreePage BTreePage::openDecoded(SlottedPage source, utils::OwnedStdVector<BTreePageEntry> &decodedEntries) {
        BTreePage page(std::move(source));
        auto validated = page.packedEntries();
        decodedEntries = std::move(validated);
        return page;
    }

    BTreePage BTreePage::openDecoded(const PageOwnerHeader &owner, std::span<const std::byte> bytes,
            utils::OwnedStdVector<BTreePageEntry> &decodedEntries) {
        return openDecoded(SlottedPage::open(owner, bytes), decodedEntries);
    }

    BTreePage BTreePage::openDecoded(SlottedPage source, std::vector<BTreePageEntry> &decodedEntries) {
        BTreePage page(std::move(source));
        auto validated = page.packedEntries();
        decodedEntries = std::move(validated);
        return page;
    }

	BTreePage BTreePage::openDecoded(const PageOwnerHeader &owner, std::span<const std::byte> bytes,
			std::vector<BTreePageEntry> &decodedEntries) {
		BTreePage page(SlottedPage::open(owner, bytes));
		auto validated = page.packedEntries();
		decodedEntries = std::move(validated);
		return page;
	}

	BTreePageHeader BTreePage::header() const {
		const SlottedSlotReference reference{page_.pageId(), 0, 1};
		return BTreePageHeaderCodec::decode(page_.read(reference));
	}

	SlottedSlotReference BTreePage::validatePackedEnvelope() const {
		const std::vector<SlottedSlotReference> slots = page_.liveSlots();
		if (slots.size() != 2U) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable packed B+Tree page must contain one header and one packed payload slot");
		}
		const uint8_t invalidHeaderSlot = static_cast<uint8_t>(slots.front().slot_id != 0U) |
										  static_cast<uint8_t>(slots.front().generation != 1U);
		if (invalidHeaderSlot != 0U) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable B+Tree page has an invalid stable header slot");
		}
		if (slots[1].slot_id != 1U || slots[1].generation != 1U) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable packed B+Tree page has an invalid payload slot");
		}
		const BTreePageHeader current = header();
		const uint8_t invalidOwner = static_cast<uint8_t>(page_.ownerKind() != PageOwnerKind::POK_INDEX) |
									 static_cast<uint8_t>(page_.ownerId() != current.index_id) |
									 static_cast<uint8_t>(page_.pageId() != current.page_id) |
									 static_cast<uint8_t>(page_.pageGeneration() != current.generation);
		if (invalidOwner != 0U) {
			throwBTreePageError(FormatError::FERR_INVALID_STATE,
								"relocatable B+Tree page owner disagrees with its persisted header");
		}
		if (PackedBTreePageCodec::entryCount(page_.read(slots[1])) != current.entry_count) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable B+Tree page entry count disagrees with its packed payload");
		}
		return slots[1];
	}

	utils::OwnedStdVector<BTreePageEntry> BTreePage::packedEntries() const {
		const SlottedSlotReference payload = validatePackedEnvelope();
		const BTreePageHeader current = header();
		return PackedBTreePageCodec::decode(current.kind, page_.read(payload), current.leaf_value_layout);
	}

	void BTreePage::writePackedEntries(std::span<const BTreePageEntry> entries) {
		const std::vector<SlottedSlotReference> slots = page_.liveSlots();
		if (slots.size() != 2U || slots[1].slot_id != 1U || slots[1].generation != 1U) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
									"relocatable packed B+Tree page has no stable payload slot");
		}
		const BTreePageHeader current = header();
		page_.overwrite(slots[1], PackedBTreePageCodec::encode(current.kind, entries, current.leaf_value_layout));
	}

	void BTreePage::validateLayout() const {
		// packedEntries() validates the stable slots, owner identity, packed entry
		// count, ordering, and internal child references in one canonical decode.
		static_cast<void>(packedEntries());
	}

	utils::OwnedStdVector<BTreePageEntry> BTreePage::entries() const {
		// packedEntries performs the same complete validation as validateLayout.
		// Return that validated decode instead of decoding once more.
		return packedEntries();
	}

	std::optional<BTreePageEntry> BTreePage::find(std::span<const std::byte> key) const {
		const SlottedSlotReference payload = validatePackedEnvelope();
		const BTreePageHeader current = header();
		return PackedBTreePageCodec::find(current.kind, page_.read(payload), key, current.leaf_value_layout);
	}

	BTreePage BTreePage::rebindOwner(const PageOwnerHeader &owner) const {
		const BTreePageHeader current = header();
		const uint8_t invalidOwner = static_cast<uint8_t>(owner.kind != PageOwnerKind::POK_INDEX) |
									 static_cast<uint8_t>(owner.owner_id != current.index_id) |
									 static_cast<uint8_t>(owner.page_id != current.page_id) |
									 static_cast<uint8_t>(!owner.generation.isValid()) |
									 static_cast<uint8_t>(!owner.committed_lsn.isValid());
		if (invalidOwner != 0U) {
			throwBTreePageError(FormatError::FERR_INVALID_STATE,
								"relocatable B+Tree page relocation owner does not preserve its logical identity");
		}
		BTreePageHeader rebound = current;
		rebound.generation = owner.generation;
		rebound.entry_count = 0;
		// The immutable source is decoded and authenticated once. Repacking the
		// complete ordered page avoids repeatedly decoding every growing prefix.
		const auto validatedEntries = entries();
		return BTreePage::createWithEntries(owner, rebound, validatedEntries);
	}

	void BTreePage::writeHeader(BTreePageHeader updated) {
		const SlottedSlotReference reference{page_.pageId(), 0, 1};
		page_.overwrite(reference, BTreePageHeaderCodec::encode(updated));
	}

	void BTreePage::insert(BTreePageEntry entry) {
		// packedEntries authenticates the entire source, including ordering.
		// Reuse that decode for duplicate detection and the mutation itself.
		utils::OwnedStdVector<BTreePageEntry> entries = packedEntries();
		const auto insertion = std::lower_bound(entries.begin(), entries.end(), entry.key,
				[](const BTreePageEntry &candidate, const std::vector<std::byte> &key) {
					return lessKey(candidate.key, key);
				});
		if (insertion != entries.end() && sameKey(insertion->key, entry.key)) {
			throwBTreePageError(FormatError::FERR_INVALID_STATE,
								"relocatable B+Tree page rejects duplicate separator keys");
		}
		BTreePageHeader updated = header();
		if (updated.kind == BTreeNodeKind::BPN_INTERNAL && !PageId(entry.value_or_child).isValid()) {
			throwBTreePageError(FormatError::FERR_INVALID_HEADER,
								"relocatable internal B+Tree entry requires a stable child Page ID");
		}
		entries.insert(insertion, std::move(entry));
		writePackedEntries(entries);
		++updated.entry_count;
		writeHeader(updated);
	}

	void BTreePage::erase(std::span<const std::byte> key) {
		utils::OwnedStdVector<BTreePageEntry> entries = packedEntries();
		const auto iterator =
				std::lower_bound(entries.begin(), entries.end(), key,
								 [](const BTreePageEntry &candidate, std::span<const std::byte> candidateKey) {
									 return lessKey(candidate.key, candidateKey);
								 });
		if (iterator == entries.end() || !sameKey(iterator->key, key)) {
			throwBTreePageError(FormatError::FERR_INVALID_STATE, "relocatable B+Tree page cannot erase an absent key");
		}
		entries.erase(iterator);
		writePackedEntries(entries);
		BTreePageHeader updated = header();
		--updated.entry_count;
		writeHeader(updated);
	}

	std::vector<std::byte> BTreePage::serialize() const {
		validateLayout();
		return page_.serialize();
	}

	const SlottedPage &BTreePage::slottedPage() const { return page_; }

} // namespace graph::storage
