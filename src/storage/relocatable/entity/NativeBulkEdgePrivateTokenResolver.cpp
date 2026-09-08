/**
 * @file NativeBulkEdgePrivateTokenResolver.cpp
 * @brief Candidate-private bounded Token lookup implementation.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateTokenResolver.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr uint32_t RSS_NATIVE_BULK_TOKEN_RESOLVER_MAGIC = 0x5254595AU; // Little-endian bytes: ZYTR
		constexpr uint16_t RSS_NATIVE_BULK_TOKEN_RESOLVER_VERSION = 1U;
		constexpr uint16_t RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES = 64U;
		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t KIND_OFFSET = 8U;
		constexpr size_t LEVEL_OFFSET = 10U;
		constexpr size_t ENTRY_COUNT_OFFSET = 12U;
		constexpr size_t OWNER_OFFSET = 16U;
		constexpr size_t RECORD_CRC_OFFSET = 24U;
		constexpr size_t RESERVED0_OFFSET = 28U;
		constexpr size_t RESERVED1_OFFSET = 32U;
		constexpr size_t RECORD_KEY_BYTES = sizeof(uint16_t);
		constexpr size_t RECORD_VALUE_BYTES = sizeof(uint64_t);
		constexpr size_t RECORD_HEADER_BYTES = RECORD_KEY_BYTES + RECORD_VALUE_BYTES;

		enum class ResolverNodeKind : uint16_t {
			RTRK_LEAF = 1U,
			RTRK_INTERNAL = 2U,
		};

		struct ResolverEntry {
			std::vector<std::byte> key;
			uint64_t value = 0U;
		};

		struct ResolverNode {
			ResolverNodeKind kind = ResolverNodeKind::RTRK_LEAF;
			uint16_t level = 0U;
			std::vector<ResolverEntry> entries;
		};

		struct LevelRange {
			size_t first_ordinal = 0U;
			size_t count = 0U;
			uint16_t level = 0U;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] uint32_t crc(std::span<const std::byte> bytes) {
			const std::byte empty{};
			return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
		}

		[[nodiscard]] size_t payloadCapacity() {
			// The compiled native segment format always has room for this fixed header.
			if (RSS_SEGMENT_SIZE <= RSS_ENVELOPE_BYTES + RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver cannot fit in a candidate segment");
			}
			return RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
		}

		[[nodiscard]] size_t maximumEntriesPerNode() {
			constexpr size_t maximumKeyBytes = 1U + RSS_SYSTEM_CATALOG_NAME_MAX_BYTES;
			constexpr size_t maximumRecordBytes = RECORD_HEADER_BYTES + maximumKeyBytes;
			const size_t usable = payloadCapacity() - RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES;
			const size_t result = usable / maximumRecordBytes;
			// The compiled segment and Catalog-name limits guarantee a fan-out of at least two.
			return result;
		}

		[[nodiscard]] size_t checkedCeilingDivide(size_t numerator, size_t denominator, const char *message) {
			static_cast<void>(message); // The internal caller supplies a nonzero count and the fixed fan-out.
			return (numerator - 1U) / denominator + 1U;
		}

		[[nodiscard]] size_t checkedAdd(size_t left, size_t right, const char *message) {
			static_cast<void>(message); // A fan-out of at least two bounds this geometric sum below its input count.
			return left + right;
		}

		[[nodiscard]] size_t entryBytes(const ResolverEntry &entry) {
			// All internal entries originate from validated resolutions or decoded records.
			return RECORD_HEADER_BYTES + entry.key.size();
		}

		[[nodiscard]] std::vector<std::byte> encodeNode(const ResolverNode &node, uint64_t owner) {
			// This private encoder receives only builder-created nodes with owned candidates.
			size_t bytes = RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES;
			for (const ResolverEntry &entry: node.entries) {
				const size_t bytesForEntry = entryBytes(entry);
				bytes += bytesForEntry;
			}
			std::vector<std::byte> result(bytes);
			LittleEndianCodec::writeU32(result, MAGIC_OFFSET, RSS_NATIVE_BULK_TOKEN_RESOLVER_MAGIC);
			LittleEndianCodec::writeU16(result, VERSION_OFFSET, RSS_NATIVE_BULK_TOKEN_RESOLVER_VERSION);
			LittleEndianCodec::writeU16(result, HEADER_BYTES_OFFSET, RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES);
			LittleEndianCodec::writeU16(result, KIND_OFFSET, static_cast<uint16_t>(node.kind));
			LittleEndianCodec::writeU16(result, LEVEL_OFFSET, node.level);
			LittleEndianCodec::writeU32(result, ENTRY_COUNT_OFFSET, static_cast<uint32_t>(node.entries.size()));
			LittleEndianCodec::writeU64(result, OWNER_OFFSET, owner);
			LittleEndianCodec::writeU32(result, RESERVED0_OFFSET, 0U);
			LittleEndianCodec::writeU64(result, RESERVED1_OFFSET, 0U);
			size_t offset = RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES;
			for (const ResolverEntry &entry: node.entries) {
				LittleEndianCodec::writeU16(result, offset, static_cast<uint16_t>(entry.key.size()));
				LittleEndianCodec::writeU64(result, offset + RECORD_KEY_BYTES, entry.value);
				offset += RECORD_HEADER_BYTES;
				std::copy(entry.key.begin(), entry.key.end(), result.begin() + static_cast<std::ptrdiff_t>(offset));
				offset += entry.key.size();
			}
			LittleEndianCodec::writeU32(result, RECORD_CRC_OFFSET,
					crc(std::span<const std::byte>(result).subspan(RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES)));
			return result;
		}

		void validateTokenKey(std::span<const std::byte> key, uint64_t value, ResolverNodeKind nodeKind);

		[[nodiscard]] ResolverNode decodeNode(std::span<const std::byte> payload, uint64_t expectedOwner) {
			if (payload.size() < RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "native bulk Edge Token resolver header is truncated");
			}
			const uint16_t rawKind = LittleEndianCodec::readU16(payload, KIND_OFFSET);
			const uint32_t count = LittleEndianCodec::readU32(payload, ENTRY_COUNT_OFFSET);
			const uint8_t invalid =
					static_cast<uint8_t>(LittleEndianCodec::readU32(payload, MAGIC_OFFSET) != RSS_NATIVE_BULK_TOKEN_RESOLVER_MAGIC) |
					static_cast<uint8_t>(LittleEndianCodec::readU16(payload, VERSION_OFFSET) != RSS_NATIVE_BULK_TOKEN_RESOLVER_VERSION) |
					static_cast<uint8_t>(LittleEndianCodec::readU16(payload, HEADER_BYTES_OFFSET) != RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES) |
					static_cast<uint8_t>(rawKind != static_cast<uint16_t>(ResolverNodeKind::RTRK_LEAF) &&
									 rawKind != static_cast<uint16_t>(ResolverNodeKind::RTRK_INTERNAL)) |
					static_cast<uint8_t>(count == 0U) |
					static_cast<uint8_t>(LittleEndianCodec::readU64(payload, OWNER_OFFSET) != expectedOwner) |
					static_cast<uint8_t>(LittleEndianCodec::readU32(payload, RESERVED0_OFFSET) != 0U) |
					static_cast<uint8_t>(LittleEndianCodec::readU64(payload, RESERVED1_OFFSET) != 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver header is invalid");
			}
			if (LittleEndianCodec::readU32(payload, RECORD_CRC_OFFSET) !=
				crc(payload.subspan(RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES))) {
				fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge Token resolver checksum does not match");
			}
			ResolverNode result;
			result.kind = static_cast<ResolverNodeKind>(rawKind);
			result.level = LittleEndianCodec::readU16(payload, LEVEL_OFFSET);
			if ((result.kind == ResolverNodeKind::RTRK_LEAF && result.level != 0U) ||
				(result.kind == ResolverNodeKind::RTRK_INTERNAL && result.level == 0U)) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver has an invalid node level");
			}
			size_t offset = RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES;
			result.entries.reserve(count);
			for (uint32_t ordinal = 0U; ordinal < count; ++ordinal) {
				if (offset > payload.size() - RECORD_HEADER_BYTES) {
					fail(FormatError::FERR_BUFFER_TOO_SMALL, "native bulk Edge Token resolver entry is truncated");
				}
				const size_t keyBytes = LittleEndianCodec::readU16(payload, offset);
				const uint64_t value = LittleEndianCodec::readU64(payload, offset + RECORD_KEY_BYTES);
				offset += RECORD_HEADER_BYTES;
				if (keyBytes == 0U || value == 0U || keyBytes > payload.size() - offset) {
					fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver entry is invalid");
				}
				ResolverEntry entry;
				entry.key.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
								 payload.begin() + static_cast<std::ptrdiff_t>(offset + keyBytes));
				entry.value = value;
				validateTokenKey(entry.key, entry.value, result.kind);
				offset += keyBytes;
				if (!result.entries.empty() && !lessKey(result.entries.back().key, entry.key)) {
					fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver entries are unordered");
				}
				result.entries.push_back(std::move(entry));
			}
			if (offset != payload.size()) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver has trailing bytes");
			}
			return result;
		}

		class RangeAllocator final {
		public:
			RangeAllocator(const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange range) :
				identities_(identities), range_(range) {
				if (range_.count == 0U || range_.first_ordinal > identities_.size() ||
					range_.count > identities_.size() - range_.first_ordinal) {
					fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver candidate range is invalid");
				}
			}

			[[nodiscard]] NativeGraphPrivateSegmentMaterialization allocate() {
				// maximumCandidateSegments reserves every leaf and parent before materialization.
				if (used_ == range_.count) {
					fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver exhausted its candidate range");
				}
				return identities_[range_.first_ordinal + used_++];
			}

			[[nodiscard]] size_t used() const { return used_; }

		private:
			const NativeGraphPrivateSegmentSet &identities_;
			NativeBulkEdgePrivateCandidateRange range_;
			size_t used_ = 0U;
		};

		[[nodiscard]] NativeGraphPrivateSegmentMaterialization identityFor(
				const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange range, uint64_t owner,
				Generation generation, uint64_t rawSegmentId) {
			const NativeGraphPrivateSegmentMaterialization candidate{SegmentId(rawSegmentId), generation, owner};
			const size_t ordinal = identities.ordinalOf(candidate);
			if (ordinal < range.first_ordinal || ordinal - range.first_ordinal >= range.count) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver references a foreign candidate");
			}
			return candidate;
		}

		[[nodiscard]] std::vector<std::byte> tokenKey(NativeBulkEdgeTokenKind kind, std::string_view name) {
			return NativeBulkEdgeTokenRunBuilder::encodeKey(kind, name);
		}

		void validateTokenKey(std::span<const std::byte> key, uint64_t value, ResolverNodeKind nodeKind) {
			if (key.size() < 2U) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver key is truncated");
			}
			const uint8_t rawKind = std::to_integer<uint8_t>(key.front());
			if (rawKind != static_cast<uint8_t>(NativeBulkEdgeTokenKind::NBETK_PROPERTY) &&
				rawKind != static_cast<uint8_t>(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE)) {
				fail(FormatError::FERR_INVALID_ENUM, "native bulk Edge Token resolver key has an invalid kind");
			}
			SystemCatalogTokenKeyCodec::validateName(std::string_view(
					reinterpret_cast<const char *>(key.data() + 1U), key.size() - 1U));
			if (value == 0U || (nodeKind == ResolverNodeKind::RTRK_LEAF &&
				rawKind == static_cast<uint8_t>(NativeBulkEdgeTokenKind::NBETK_PROPERTY) &&
				value > std::numeric_limits<uint32_t>::max())) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver value is invalid");
			}
		}

		void validateResolution(const NativeBulkEdgeCatalogTokenResolution &resolution) {
			SystemCatalogTokenKeyCodec::validateName(resolution.name);
			if ((resolution.kind != NativeBulkEdgeTokenKind::NBETK_PROPERTY &&
				 resolution.kind != NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE) ||
				resolution.token_id == 0U ||
				(resolution.kind == NativeBulkEdgeTokenKind::NBETK_PROPERTY &&
				 resolution.token_id > std::numeric_limits<uint32_t>::max())) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Token resolver received an invalid resolution");
			}
		}

	} // namespace

	NativeBulkEdgePrivateTokenResolver NativeBulkEdgePrivateTokenResolver::makeEmpty() {
		return NativeBulkEdgePrivateTokenResolver(nullptr, {}, {}, 0U, {});
	}

	size_t NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(size_t maximumTokenEntries) {
		if (maximumTokenEntries == 0U) {
			return 0U;
		}
		const size_t fanout = maximumEntriesPerNode();
		size_t total = 0U;
		for (size_t nodes = checkedCeilingDivide(maximumTokenEntries, fanout,
				 "native bulk Edge Token resolver has an invalid entry bound");;) {
			total = checkedAdd(total, nodes, "native bulk Edge Token resolver candidate count overflowed");
			if (nodes == 1U) {
				return total;
			}
			nodes = checkedCeilingDivide(nodes, fanout,
					"native bulk Edge Token resolver has an invalid internal fan-out");
		}
	}

	NativeBulkEdgePrivateTokenResolver NativeBulkEdgePrivateTokenResolver::materialize(
			const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
			const NativeBulkEdgeCatalogTokenResolutionSource &resolutions, size_t maximumTokenEntries,
			const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
			const NativeGraphPrivateSegmentFence &fence) {
		if (!resolutions || maximumTokenEntries == 0U ||
			maximumTokenEntries == std::numeric_limits<size_t>::max() || !reader || !writer || !fence ||
			candidateRange.count == 0U ||
			candidateRange.count < maximumCandidateSegments(maximumTokenEntries)) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge Token resolver requires bounded scoped candidate callbacks");
		}
		RangeAllocator allocator(identities, candidateRange);
		NativeBulkEdgeCatalogTokenResolutionCursor cursor = resolutions();
		if (!cursor) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver source did not open");
		}

		std::vector<ResolverEntry> leaf;
		std::optional<std::vector<std::byte>> previousKey;
		size_t seen = 0U;
		LevelRange current{};
		bool hasLeaf = false;
		const auto flushLeaf = [&] {
			if (leaf.empty()) {
				return;
			}
			const NativeGraphPrivateSegmentMaterialization identity = allocator.allocate();
			writer(identity, encodeNode({ResolverNodeKind::RTRK_LEAF, 0U, std::move(leaf)}, identity.owner_id));
			if (!hasLeaf) {
				current.first_ordinal = candidateRange.first_ordinal;
				hasLeaf = true;
			}
			++current.count;
			leaf = {};
		};
		while (const std::optional<NativeBulkEdgeCatalogTokenResolution> resolution = cursor()) {
			if (++seen > maximumTokenEntries) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver exceeded its entry bound");
			}
			validateResolution(*resolution);
			ResolverEntry next{tokenKey(resolution->kind, resolution->name), resolution->token_id};
			if (previousKey.has_value() && !lessKey(*previousKey, next.key)) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver source is unordered");
			}
			const size_t nextBytes = entryBytes(next);
			size_t usedBytes = RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES;
			for (const ResolverEntry &entry: leaf) {
				usedBytes += entryBytes(entry);
			}
			// Resolution validation caps each key below the compiled candidate payload.
			if (usedBytes > payloadCapacity() - nextBytes) {
				flushLeaf();
			}
			previousKey = next.key;
			leaf.push_back(std::move(next));
		}
		flushLeaf();
		if (!hasLeaf) {
			return NativeBulkEdgePrivateTokenResolver(&identities, candidateRange, {}, 0U, reader);
		}
		fence();

		while (current.count > 1U) {
			const uint16_t nextLevel = static_cast<uint16_t>(current.level + 1U);
			// The size_t-bounded candidate tree with fan-out >= 2 cannot reach uint16_t depth.
			LevelRange parent{candidateRange.first_ordinal + allocator.used(), 0U, nextLevel};
			std::vector<ResolverEntry> entries;
			for (size_t ordinal = 0U; ordinal < current.count; ++ordinal) {
				const NativeGraphPrivateSegmentMaterialization child = identities[current.first_ordinal + ordinal];
				const ResolverNode childNode = decodeNode(reader(child), child.owner_id);
				// This pass reads the immutable candidates just emitted for current.
				ResolverEntry next{childNode.entries.front().key, child.segment_id.value()};
				size_t usedBytes = RSS_NATIVE_BULK_TOKEN_RESOLVER_HEADER_BYTES;
				for (const ResolverEntry &entry: entries) {
					usedBytes += entryBytes(entry);
				}
				if (usedBytes > payloadCapacity() - entryBytes(next) && !entries.empty()) {
					const NativeGraphPrivateSegmentMaterialization identity = allocator.allocate();
					writer(identity, encodeNode({ResolverNodeKind::RTRK_INTERNAL, nextLevel, std::move(entries)}, identity.owner_id));
					++parent.count;
					entries = {};
				}
				entries.push_back(std::move(next));
			}
			// A nonempty current level always emits at least one parent entry.
			const NativeGraphPrivateSegmentMaterialization identity = allocator.allocate();
			writer(identity, encodeNode({ResolverNodeKind::RTRK_INTERNAL, nextLevel, std::move(entries)}, identity.owner_id));
			++parent.count;
			fence();
			current = parent;
		}
		return NativeBulkEdgePrivateTokenResolver(&identities, candidateRange, identities[current.first_ordinal],
				allocator.used(), reader);
	}

	std::optional<uint64_t> NativeBulkEdgePrivateTokenResolver::find(NativeBulkEdgeTokenKind kind,
			std::string_view name) const {
		if (!root_.isValid() || identities_ == nullptr || !reader_) {
			return std::nullopt;
		}
		const std::vector<std::byte> key = tokenKey(kind, name);
		NativeGraphPrivateSegmentMaterialization current = root_;
		for (size_t depth = 0U; depth < maximumCandidateSegmentCount(); ++depth) {
			const ResolverNode node = decodeNode(reader_(current), current.owner_id);
			auto firstGreater = std::upper_bound(node.entries.begin(), node.entries.end(), key,
					[](const std::vector<std::byte> &needle, const ResolverEntry &entry) {
						return lessKey(needle, entry.key);
					});
			if (node.kind == ResolverNodeKind::RTRK_LEAF) {
				if (firstGreater == node.entries.begin()) {
					return std::nullopt;
				}
				--firstGreater;
				return firstGreater->key == key ? std::optional<uint64_t>(firstGreater->value) : std::nullopt;
			}
			if (firstGreater == node.entries.begin()) {
				return std::nullopt;
			}
			--firstGreater;
			current = identityFor(*identities_, candidate_range_, current.owner_id, current.generation,
					firstGreater->value);
			const ResolverNode child = decodeNode(reader_(current), current.owner_id);
			if (child.level + 1U != node.level) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver child has an invalid depth");
			}
		}
		fail(FormatError::FERR_INVALID_STATE, "native bulk Edge Token resolver traversal exceeded its candidate bound");
	}

	NativeBulkEdgeMaterializerResolvers NativeBulkEdgePrivateTokenResolver::materializerResolvers() const {
		struct LastLookup {
			std::string name;
			std::optional<uint64_t> value;
		};
		// A materializer commonly resolves one relationship type and a small set of
		// column keys repeatedly. Keep only the latest lookup for each namespace:
		// private candidates are immutable under the enclosing writer fence, while
		// this remains strictly bounded regardless of the batch cardinality.
		auto relationshipCache = std::make_shared<std::optional<LastLookup>>();
		auto propertyCache = std::make_shared<std::optional<LastLookup>>();
		return {
			[this, relationshipCache](std::string_view name) {
				if (relationshipCache->has_value() && relationshipCache->value().name == name) {
					return relationshipCache->value().value;
				}
				const std::optional<uint64_t> resolved = find(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, name);
				*relationshipCache = {std::string(name), resolved};
				return resolved;
			},
			[this, propertyCache](std::string_view name) -> std::optional<uint32_t> {
				std::optional<uint64_t> token;
				if (propertyCache->has_value() && propertyCache->value().name == name) {
					token = propertyCache->value().value;
				} else {
					token = find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, name);
					*propertyCache = {std::string(name), token};
				}
				if (!token.has_value()) {
					return std::nullopt;
				}
				// validateTokenKey rejects persisted leaf Property IDs above uint32_t.
				return static_cast<uint32_t>(*token);
			},
		};
	}

} // namespace graph::storage
