/**
 * @file LargeValueStore.cpp
 * @brief Atomic generic LargeValue materialization and checked snapshot reads.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/detail/ScopedPropertyBuffers.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "storage/relocatable/property/PoolPreparationBatch.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <deque>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>

#if ZYX_HAS_LZ4
#include <lz4.h>
#endif
#if ZYX_HAS_ZSTD
#include <zstd.h>
#endif

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {

		struct PlannedChunk {
			size_t source_offset = 0U;
			uint64_t uncompressed_offset = 0U;
			uint32_t uncompressed_bytes = 0U;
			LargeValueCodec codec = LargeValueCodec::LVC_RAW;
			std::vector<std::byte> stored;
			uint16_t page_count = 0U;
			PageId first_page_id{};
		};

		struct StreamedChunkPlan {
			LargeValueCodec codec = LargeValueCodec::LVC_RAW;
			uint16_t page_count = 0U;
		};

		struct PlannedManifestNode {
			LargeValueManifestNodeKind kind = LargeValueManifestNodeKind::LVMNK_ROOT;
			uint16_t level = 0U;
			size_t first_chunk_index = 0U;
			size_t chunk_count = 0U;
			std::vector<size_t> children;
			PageId page_id{};
		};

		struct PlannedManifestTree {
			std::vector<PlannedManifestNode> nodes;
			size_t root_index = 0U;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] size_t checkedReadBufferSum(size_t first, size_t second) {
			if (second > std::numeric_limits<size_t>::max() - first)
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue read buffers overflowed");
			return first + second;
		}

		[[nodiscard]] bool isLargeValueKind(LargeValueKind kind) {
			return kind == LargeValueKind::LVK_BYTES || kind == LargeValueKind::LVK_UTF8_TEXT;
		}

		void validateMediaType(const std::string &mediaType) {
			if (mediaType.empty() || mediaType.size() > RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue media type must be nonempty and within the format policy bound");
			}
			for (const unsigned char character: mediaType) {
				if (character < 0x20U || character > 0x7EU) {
					fail(FormatError::FERR_INVALID_HEADER, "LargeValue media type must be printable ASCII metadata");
				}
			}
		}

		void validateCompressionOptions(const LargeValueCompressionOptions &options) {
			if (options.logical_chunk_ends.size() > 4096U || options.minimum_savings_percent > 100U || options.zstd_level < 1 || options.zstd_level > 22 ||
				(options.codec != LargeValueCodec::LVC_RAW && options.codec != LargeValueCodec::LVC_LZ4 &&
				 options.codec != LargeValueCodec::LVC_ZSTD)) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue compression policy is invalid");
			}
		}

		[[nodiscard]] uint32_t extentBytes(LargeValueExtentClass extentClass) {
			switch (extentClass) {
				case LargeValueExtentClass::LVEC_16K:
					return RSS_LARGE_VALUE_EXTENT_16K_BYTES;
				case LargeValueExtentClass::LVEC_32K:
					return RSS_LARGE_VALUE_EXTENT_32K_BYTES;
				case LargeValueExtentClass::LVEC_64K:
					return RSS_LARGE_VALUE_EXTENT_64K_BYTES;
				case LargeValueExtentClass::LVEC_128K:
					return RSS_LARGE_VALUE_EXTENT_128K_BYTES;
			}
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue extent class is invalid");
		}

		void validateWriteOptions(const LargeValueWriteOptions &options, LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(!options.first_data_slot.isValid()) |
									static_cast<uint8_t>(!options.last_data_slot.isValid()) |
									static_cast<uint8_t>(options.first_data_slot > options.last_data_slot) |
									static_cast<uint8_t>(!options.first_manifest_slot.isValid()) |
									static_cast<uint8_t>(!options.last_manifest_slot.isValid()) |
									static_cast<uint8_t>(options.first_manifest_slot > options.last_manifest_slot) |
									static_cast<uint8_t>(!isLargeValueKind(options.kind)) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue write requires valid data and manifest pool ranges, kind, and checkpoint LSN");
			}
			validateMediaType(options.media_type);
			validateCompressionOptions(options.compression);
			static_cast<void>(extentBytes(options.extent_class));
			if (options.maximum_dedup_candidates == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue write requires a nonzero content-index candidate bound");
			}
		}

		void appendBigEndianU16(std::vector<std::byte> &bytes, uint16_t value) {
			bytes.push_back(static_cast<std::byte>(value >> 8U));
			bytes.push_back(static_cast<std::byte>(value));
		}

		void appendBigEndianU64(std::vector<std::byte> &bytes, uint64_t value) {
			for (int shift = 56; shift >= 0; shift -= 8) {
				bytes.push_back(static_cast<std::byte>(value >> shift));
			}
		}

		[[nodiscard]] std::vector<std::byte>
		contentIdentityPrefix(LargeValueKind kind, uint64_t logicalBytes,
							  const std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> &contentHash,
							  const std::string &mediaType) {
			// The key is self-delimiting and contains all logical identity fields.
			// In particular, no truncated hash is ever used as a deduplication key.
			std::vector<std::byte> key;
			key.reserve(1U + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t) + contentHash.size() +
						mediaType.size());
			key.push_back(std::byte{1U});
			appendBigEndianU16(key, static_cast<uint16_t>(kind));
			appendBigEndianU16(key, static_cast<uint16_t>(mediaType.size()));
			appendBigEndianU64(key, logicalBytes);
			key.insert(key.end(), contentHash.begin(), contentHash.end());
			key.insert(key.end(), reinterpret_cast<const std::byte *>(mediaType.data()),
					   reinterpret_cast<const std::byte *>(mediaType.data() + mediaType.size()));
			return key;
		}

		[[nodiscard]] std::vector<std::byte> makeContentIndexKey(const LargeValueRef &reference) {
			if (!reference.value_id.isValid()) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue content-index key requires a valid value identity");
			}
			std::vector<std::byte> key = contentIdentityPrefix(reference.kind, reference.logical_bytes,
															   reference.content_hash, reference.media_type);
			appendBigEndianU64(key, reference.value_id.value());
			return key;
		}

		[[nodiscard]] bool startsWith(std::span<const std::byte> bytes, std::span<const std::byte> prefix) {
			return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin());
		}

		[[nodiscard]] uint64_t decodeBigEndianU64(std::span<const std::byte> bytes) {
			if (bytes.size() !=
				sizeof(uint64_t)) { // ZYX_COV_EXCL_LINE: caller slices the key after verifying its exact suffix width.
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue content-index value suffix has an invalid width");
			}
			uint64_t value = 0U;
			for (const std::byte byte: bytes) {
				value = (value << 8U) | static_cast<uint8_t>(byte);
			}
			return value;
		}

		[[nodiscard]] LargeValueContentIndexPublicationPlan
		makeContentIndexPublicationPlan(BTreePageStore &pages, RelocatableMetadataStore &metadata,
									const MetadataSnapshotPin &snapshot,
									std::span<const BTreeScalarIndexOperation> operations, PhysicalSlotId firstSlot,
									PhysicalSlotId lastSlot, bool forceFreshRebuild = false) {
			if (operations.empty() || !firstSlot.isValid() || !lastSlot.isValid() || firstSlot > lastSlot) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue content-index publication requires operations and a valid allocation range");
			}
			LargeValueContentIndexPublicationPlan result;
			if (metadata.findIndexRootOptional(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID).has_value()) {
				BTreeStructuralMutationPlan plan = pages.planScalarIndexTransactionMutation(
						snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID, operations, true, forceFreshRebuild);
				if (plan.expected_root.index_id != RSS_LARGE_VALUE_CONTENT_INDEX_ID || !plan.preparer) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue content-index mutation lost its pinned root or typed preparer");
				}
				result.mutation.index_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID;
				result.mutation.expected_root = plan.expected_root;
				result.mutation.pool_requests = std::move(plan.pool_requests);
				if (plan.new_pool_mutation.has_value()) {
					result.mutation.new_pool_requests.push_back({firstSlot, lastSlot, plan.new_pool_mutation->owner_id,
																 plan.new_pool_mutation->appended_page_count});
				}
				for (const BTreeNewIndexTreePoolMutation &fresh: plan.additional_new_pool_mutations) {
					result.mutation.new_pool_requests.push_back(
							{firstSlot, lastSlot, fresh.owner_id, fresh.appended_page_count});
				}
				if (plan.replacement_root.has_value()) {
					const IndexTreeMutationAppendedPageReference root = *plan.replacement_root;
					const bool newlyReserved = root.pool_ordinal >= result.mutation.pool_requests.size();
					const size_t poolOrdinal = newlyReserved ? root.pool_ordinal - result.mutation.pool_requests.size()
															 : root.pool_ordinal;
					const size_t poolCount = newlyReserved ? result.mutation.new_pool_requests.size()
																	 : result.mutation.pool_requests.size();
					if (poolOrdinal >= poolCount) {
						fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue content-index root references an absent materialization pool");
					}
					result.mutation.replacement_root = {poolOrdinal, root.appended_page_ordinal, newlyReserved};
				}
				result.mutation.stable_replacement_root = plan.stable_replacement_root;
				result.preparer = std::move(plan.preparer);
				return result;
			}

			std::vector<BTreePageEntry> entries;
			entries.reserve(operations.size());
			for (const BTreeScalarIndexOperation &operation: operations) {
				entries.push_back({operation.key, operation.value});
			}
			std::ranges::sort(entries, {}, &BTreePageEntry::key);
			if (std::ranges::adjacent_find(entries, {}, &BTreePageEntry::key) != entries.end()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue content-index publication contains duplicate content identities");
			}
			BTreeInitialTreeMutationPlan plan =
					pages.planInitialTreeMutation(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID, entries);
			if (plan.index_id != RSS_LARGE_VALUE_CONTENT_INDEX_ID || plan.new_pool_mutations.empty() || !plan.preparer ||
				plan.root_page.pool_ordinal >= plan.new_pool_mutations.size() ||
				plan.root_page.appended_page_ordinal >=
						plan.new_pool_mutations[plan.root_page.pool_ordinal].appended_page_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue content-index initial tree lost its root or typed preparer");
			}
			result.mutation.index_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				result.mutation.new_pool_requests.push_back(
						{firstSlot, lastSlot, pool.owner_id, pool.appended_page_count});
			}
			result.mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal,
												true};
			result.preparer = std::move(plan.preparer);
			return result;
		}

		[[nodiscard]] constexpr uint64_t maximumChunkStoredBytes() {
			constexpr uint64_t pageBound = static_cast<uint64_t>(std::numeric_limits<uint16_t>::max()) *
										   RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
			return std::min(pageBound, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
		}

		[[nodiscard]] uint16_t pageCountForBytes(uint32_t bytes) {
			const uint64_t count = (static_cast<uint64_t>(bytes) + RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES - 1U) /
								   RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
			return static_cast<uint16_t>(count);
		}

		[[nodiscard]] size_t minimumSavingsBytes(size_t rawBytes, uint8_t percent) {
			const uint64_t raw = static_cast<uint64_t>(rawBytes);
			const uint64_t quotient = raw / 100U;
			const uint64_t remainder = raw % 100U;
			const uint64_t required = quotient * percent + (remainder * percent + 99U) / 100U;
			return static_cast<size_t>(std::min(required, raw));
		}

		[[nodiscard]] std::vector<std::byte> compressPayload(std::span<const std::byte> input, LargeValueCodec codec,
															 int zstdLevel) {
			if (codec == LargeValueCodec::LVC_RAW) {
				return {input.begin(), input.end()};
			}
			if (codec == LargeValueCodec::LVC_LZ4) {
#if ZYX_HAS_LZ4
				if (input.size() >
					static_cast<size_t>(
							INT_MAX)) { // ZYX_COV_EXCL_LINE: format extent caps each chunk far below INT_MAX.
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue LZ4 input exceeds the codec address bound");
				}
				const int bound = LZ4_compressBound(static_cast<int>(input.size()));
				if (bound <= 0) { // ZYX_COV_EXCL_LINE: LZ4_compressBound is positive for a validated in-range input.
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue LZ4 bound calculation failed");
				}
				std::vector<std::byte> output(static_cast<size_t>(bound));
				const int written = LZ4_compress_default(reinterpret_cast<const char *>(input.data()),
														 reinterpret_cast<char *>(output.data()),
														 static_cast<int>(input.size()), bound);
				if (written <=
					0) { // ZYX_COV_EXCL_LINE: an exact LZ4 bound and validated input cannot fail compression.
					fail(FormatError::FERR_INVALID_STATE, "LargeValue LZ4 compression failed");
				}
				output.resize(static_cast<size_t>(written));
				return output;
#else
				static_cast<void>(input);
				static_cast<void>(zstdLevel);
				fail(FormatError::FERR_UNSUPPORTED_VERSION, "LargeValue LZ4 codec is unavailable in this build");
#endif
			}
			if (codec ==
				LargeValueCodec::LVC_ZSTD) { // ZYX_COV_EXCL_LINE: writer policy accepts only RAW, LZ4, or Zstd.
#if ZYX_HAS_ZSTD
				std::vector<std::byte> output(ZSTD_compressBound(input.size()));
				const size_t written =
						ZSTD_compress(output.data(), output.size(), input.data(), input.size(), zstdLevel);
				if (ZSTD_isError(written) !=
					0U) { // ZYX_COV_EXCL_LINE: the validated level and ZSTD_compressBound output are valid.
					fail(FormatError::FERR_INVALID_STATE, "LargeValue Zstandard compression failed");
				}
				output.resize(written);
				return output;
#else
				static_cast<void>(input);
				static_cast<void>(zstdLevel);
				fail(FormatError::FERR_UNSUPPORTED_VERSION, "LargeValue Zstandard codec is unavailable in this build");
#endif
			}
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue writer received an unsupported codec");
		}

		[[nodiscard]] std::vector<std::byte> selectStoredPayload(std::span<const std::byte> input,
																 const LargeValueCompressionOptions &options) {
			std::vector<std::byte> compressed = compressPayload(input, options.codec, options.zstd_level);
			if (options.codec == LargeValueCodec::LVC_RAW ||
				(compressed.size() < input.size() &&
				 input.size() - compressed.size() >=
						 minimumSavingsBytes(input.size(), options.minimum_savings_percent))) {
				return compressed;
			}
			if (!options.allow_raw_fallback) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue compression did not meet the configured savings threshold");
			}
			return {input.begin(), input.end()};
		}

		[[nodiscard]] utils::OwnedVector<std::byte> decompressPayload(std::span<const std::byte> input, LargeValueCodec codec,
															   size_t expectedBytes) {
			if (codec == LargeValueCodec::LVC_RAW) {
				if (input.size() != expectedBytes) {
					fail(FormatError::FERR_CHECKSUM_MISMATCH,
						 "LargeValue RAW chunk length disagrees with its manifest");
				}
				return {input.begin(), input.end()};
			}
			utils::OwnedVector<std::byte> output(expectedBytes);
			if (codec == LargeValueCodec::LVC_LZ4) {
#if ZYX_HAS_LZ4
				if (expectedBytes > static_cast<size_t>(INT_MAX) ||
					input.size() >
							static_cast<size_t>(
									INT_MAX)) { // ZYX_COV_EXCL_LINE: chunk extent caps both LZ4 operands below INT_MAX.
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "LargeValue LZ4 output exceeds the codec address bound");
				}
				const int decoded = LZ4_decompress_safe(
						reinterpret_cast<const char *>(input.data()), reinterpret_cast<char *>(output.data()),
						static_cast<int>(input.size()), static_cast<int>(expectedBytes));
				if (decoded != static_cast<int>(expectedBytes)) {
					fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue LZ4 chunk decompression failed");
				}
				return output;
#else
				static_cast<void>(input);
				static_cast<void>(expectedBytes);
				fail(FormatError::FERR_UNSUPPORTED_VERSION, "LargeValue LZ4 codec is unavailable in this build");
#endif
			}
			if (codec == LargeValueCodec::LVC_ZSTD) { // ZYX_COV_EXCL_LINE: Manifest codec rejects unsupported chunk
													  // codecs before dispatch.
#if ZYX_HAS_ZSTD
				const size_t decoded = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
				if (ZSTD_isError(decoded) != 0U || decoded != expectedBytes) {
					fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue Zstandard chunk decompression failed");
				}
				return output;
#else
				static_cast<void>(input);
				static_cast<void>(expectedBytes);
				fail(FormatError::FERR_UNSUPPORTED_VERSION, "LargeValue Zstandard codec is unavailable in this build");
#endif
			}
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue reader received an unsupported codec");
		}

		[[nodiscard]] LargeValueCodec selectedCodec(std::span<const std::byte> input,
													const LargeValueCompressionOptions &options,
													const std::vector<std::byte> &stored) {
			if (options.codec == LargeValueCodec::LVC_RAW || stored.size() == input.size()) {
				return LargeValueCodec::LVC_RAW;
			}
			return options.codec;
		}

		void validateChunkEnds(const LargeValueCompressionOptions &options, uint64_t size) {
			uint64_t previous = 0U;
			if (options.logical_chunk_ends.size() > 4096U) fail(FormatError::FERR_INVALID_HEADER, "LargeValue chunk-boundary count exceeds its limit");
			for (uint64_t end : options.logical_chunk_ends) {
				if (end <= previous || end > size) fail(FormatError::FERR_INVALID_HEADER, "LargeValue chunk boundaries are not ordered bounded offsets");
				previous = end;
			}
		}

		[[nodiscard]] uint64_t nextChunkBytes(uint64_t size, uint64_t offset, uint64_t target,
			const LargeValueCompressionOptions &options) {
			const auto next = std::ranges::upper_bound(options.logical_chunk_ends, offset);
			const uint64_t end = next == options.logical_chunk_ends.end() ? size : *next;
			return std::min(target, end - offset);
		}

		[[nodiscard]] std::pair<uint64_t, uint32_t> chunkRange(size_t ordinal, uint64_t size, uint64_t target,
			const LargeValueCompressionOptions &options) {
			uint64_t begin = 0U;
			for (size_t segment = 0U; segment <= options.logical_chunk_ends.size(); ++segment) {
				const uint64_t end = segment == options.logical_chunk_ends.size() ? size : options.logical_chunk_ends[segment];
				const uint64_t span = end - begin;
				const uint64_t count = span / target + static_cast<uint64_t>(span % target != 0U);
				if (ordinal < count) {
					const uint64_t offset = begin + static_cast<uint64_t>(ordinal) * target;
					return {offset, static_cast<uint32_t>(std::min(target, end - offset))};
				}
				ordinal -= static_cast<size_t>(count); begin = end;
			}
			fail(FormatError::FERR_INVALID_STATE, "LargeValue chunk ordinal exceeds its planned ranges");
		}

		[[nodiscard]] std::vector<PlannedChunk> planChunks(std::span<const std::byte> bytes,
														   const LargeValueCompressionOptions &compression,
														   LargeValueExtentClass extentClass) {
			validateChunkEnds(compression, bytes.size());
			std::vector<PlannedChunk> chunks;
			const auto append = [&chunks](PlannedChunk &&chunk) { chunks.push_back(std::move(chunk)); };
			const uint64_t targetBytes = std::min<uint64_t>(extentBytes(extentClass), maximumChunkStoredBytes());
			size_t sourceOffset = 0U;
			uint64_t logicalOffset = 0U;
			while (sourceOffset < bytes.size()) {
				const uint64_t chunkBytes = nextChunkBytes(bytes.size(), sourceOffset, targetBytes, compression);
				const uint64_t nextOffset = logicalOffset + chunkBytes;
			if (nextOffset < logicalOffset || chunks.size() >= std::numeric_limits<uint32_t>::max()) { // reaching the chunk-ordinal limit needs at least 64 TiB of 16 KiB input and belongs to the scale gate, not a unit test.
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue exceeds the encodable chunk-ordinal range");
				}
				const uint32_t uncompressedBytes = static_cast<uint32_t>(chunkBytes);
				const std::span<const std::byte> source = bytes.subspan(sourceOffset, uncompressedBytes);
				std::vector<std::byte> stored = selectStoredPayload(source, compression);
				const uint16_t pageCount = pageCountForBytes(static_cast<uint32_t>(stored.size()));
				append({sourceOffset, logicalOffset, uncompressedBytes, selectedCodec(source, compression, stored),
						std::move(stored), pageCount, {}});
				sourceOffset += static_cast<size_t>(chunkBytes);
				logicalOffset = nextOffset;
			}
			return chunks;
		}

		// Streamed publication invokes this once per bounded logical extent. Unlike
		// planChunks(), it never retains all compressed chunks of one large value.
		template <typename Visitor>
		void visitPlannedChunks(std::span<const std::byte> bytes, const LargeValueCompressionOptions &compression,
							   LargeValueExtentClass extentClass, Visitor &&visit) {
			validateChunkEnds(compression, bytes.size());
			size_t ordinal = 0U;
			size_t sourceOffset = 0U;
			uint64_t logicalOffset = 0U;
			const uint64_t targetBytes = std::min<uint64_t>(extentBytes(extentClass), maximumChunkStoredBytes());
			while (sourceOffset < bytes.size()) {
				const uint64_t chunkBytes = nextChunkBytes(bytes.size(), sourceOffset, targetBytes, compression);
				const uint64_t nextOffset = logicalOffset + chunkBytes;
			if (nextOffset < logicalOffset || // reaching the chunk-ordinal limit needs at least 64 TiB of 16 KiB input and belongs to the scale gate, not a unit test.
					ordinal >= std::numeric_limits<uint32_t>::max()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue exceeds the encodable chunk-ordinal range");
				}
				const uint32_t uncompressedBytes = static_cast<uint32_t>(chunkBytes);
				const std::span<const std::byte> source = bytes.subspan(sourceOffset, uncompressedBytes);
				std::vector<std::byte> stored = selectStoredPayload(source, compression);
				const uint32_t storedBytes = static_cast<uint32_t>(stored.size());
				visit(StreamedChunkPlan{selectedCodec(source, compression, stored), pageCountForBytes(storedBytes)});
				sourceOffset += static_cast<size_t>(chunkBytes);
				logicalOffset = nextOffset; ++ordinal;
			}
		}

		[[nodiscard]] size_t plannedManifestNodeCount(size_t chunkCount) {
			if (chunkCount <= RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) {
				return 1U;
			}
			const auto roundedDivision = [](size_t value, size_t divisor) {
				return value / divisor + static_cast<size_t>(value % divisor != 0U);
			};
			size_t nodeCount = roundedDivision(chunkCount, RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS);
			size_t levelCount = nodeCount;
			while (levelCount > RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN) {
				levelCount = roundedDivision(levelCount, RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN);
				if (levelCount > std::numeric_limits<size_t>::max() - nodeCount) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "LargeValue manifest hierarchy exceeds the platform size bound");
				}
				nodeCount += levelCount;
			}
			if (nodeCount == std::numeric_limits<size_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "LargeValue manifest hierarchy exceeds the platform size bound");
			}
			return nodeCount + 1U; // Root is always materialized after the bounded levels.
		}

		[[nodiscard]] LargeValueCodec aggregateCodec(std::span<const PlannedChunk> chunks) {
			if (chunks.empty()) {
				return LargeValueCodec::LVC_RAW;
			}
			const LargeValueCodec first = chunks.front().codec;
			return std::ranges::all_of(chunks, [first](const PlannedChunk &chunk) { return chunk.codec == first; })
						   ? first
						   : LargeValueCodec::LVC_MIXED;
		}

		[[nodiscard]] size_t totalPlannedDataPages(std::span<const PlannedChunk> chunks) {
			size_t pageCount = 0U;
			for (const PlannedChunk &chunk: chunks) {
				pageCount += chunk.page_count;
			}
			return pageCount;
		}

		[[nodiscard]] uint64_t nodeLogicalBytes(const std::vector<PlannedChunk> &chunks, size_t firstChunk,
												size_t chunkCount) {
			const PlannedChunk &first = chunks[firstChunk];
			const PlannedChunk &last = chunks[firstChunk + chunkCount - 1U];
			const uint64_t end = last.uncompressed_offset + last.uncompressed_bytes;
			return end - first.uncompressed_offset;
		}

		[[nodiscard]] PlannedManifestTree planManifestTree(const std::vector<PlannedChunk> &chunks) {
			PlannedManifestTree tree;
			if (chunks.size() <= RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) {
				tree.nodes.push_back({LargeValueManifestNodeKind::LVMNK_ROOT, 0U, 0U, chunks.size(), {}, {}});
				return tree;
			}

			std::vector<size_t> level;
			for (size_t first = 0U; first < chunks.size();) {
				const size_t count =
						std::min(chunks.size() - first, static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS));
				tree.nodes.push_back({LargeValueManifestNodeKind::LVMNK_LEAF, 0U, first, count, {}, {}});
				level.push_back(tree.nodes.size() - 1U);
				first += count;
			}

			uint16_t levelNumber = 0U;
			while (level.size() > RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN) {
				++levelNumber;
				std::vector<size_t> next;
				for (size_t first = 0U; first < level.size();) {
					const size_t count =
							std::min(level.size() - first, static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN));
					const PlannedManifestNode &firstChild = tree.nodes[level[first]];
					size_t chunkCount = 0U;
					for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
						const size_t childChunks = tree.nodes[level[first + ordinal]].chunk_count;
						chunkCount += childChunks;
					}
					tree.nodes.push_back({LargeValueManifestNodeKind::LVMNK_INTERNAL,
										  levelNumber,
										  firstChild.first_chunk_index,
										  chunkCount,
										  {level.begin() + static_cast<std::ptrdiff_t>(first),
										   level.begin() + static_cast<std::ptrdiff_t>(first + count)},
										  {}});
					next.push_back(tree.nodes.size() - 1U);
					first += count;
				}
				level = std::move(next);
			}
			const PlannedManifestNode &firstChild = tree.nodes[level.front()];
			size_t chunkCount = 0U;
			for (const size_t child: level) {
				const size_t childChunks = tree.nodes[child].chunk_count;
				chunkCount += childChunks;
			}
			tree.nodes.push_back({LargeValueManifestNodeKind::LVMNK_ROOT,
								  static_cast<uint16_t>(levelNumber + 1U),
								  firstChild.first_chunk_index,
								  chunkCount,
								  std::move(level),
								  {}});
			tree.root_index = tree.nodes.size() - 1U;
			return tree;
		}

		[[nodiscard]] std::vector<size_t> rootFirstManifestOrder(const PlannedManifestTree &tree) {
			std::vector<size_t> order;
			order.reserve(tree.nodes.size());
			std::deque<size_t> pending;
			pending.push_back(tree.root_index);
			while (!pending.empty()) {
				const size_t current = pending.front();
				pending.pop_front();
				order.push_back(current);
				for (const size_t child: tree.nodes[current].children) {
					pending.push_back(child);
				}
			}
			return order;
		}

		[[nodiscard]] std::vector<LargeValueChunk> materializedChunks(std::span<const std::byte> bytes,
																	  const std::vector<PlannedChunk> &chunks) {
			std::vector<LargeValueChunk> result;
			result.reserve(chunks.size());
			for (size_t ordinal = 0U; ordinal < chunks.size(); ++ordinal) {
				const PlannedChunk &chunk = chunks[ordinal];
				const std::span<const std::byte> raw = bytes.subspan(chunk.source_offset, chunk.uncompressed_bytes);
				result.push_back({static_cast<uint32_t>(ordinal), chunk.codec, chunk.uncompressed_offset,
								  chunk.uncompressed_bytes, static_cast<uint32_t>(chunk.stored.size()),
								  utils::calculateCrc(raw.data(), raw.size()),
								  utils::calculateCrc(chunk.stored.data(), chunk.stored.size()), chunk.first_page_id,
								  chunk.page_count});
			}
			return result;
		}

		[[nodiscard]] std::vector<LargeValueManifest>
		buildManifestNodes(LargeValueId valueId, Generation generation, LogSequenceNumber checkpointLsn,
						   uint64_t logicalBytes,
						   const std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> &contentHash,
						   LargeValueKind kind, LargeValueCodec codec, const std::string &mediaType,
						   const std::vector<LargeValueChunk> &chunks, std::span<const PageId> pageIds) {
			std::vector<PlannedChunk> planningChunks;
			planningChunks.reserve(chunks.size());
			for (const LargeValueChunk &chunk: chunks) {
				planningChunks.push_back({0U,
										  chunk.uncompressed_offset,
										  chunk.uncompressed_bytes,
										  chunk.codec,
										  {},
										  chunk.data_page_count,
										  chunk.first_data_page_id});
			}
			PlannedManifestTree tree = planManifestTree(planningChunks);
			const std::vector<size_t> order = rootFirstManifestOrder(tree);
			// The native metadata transaction allocated this exact root-first PageId
			// sequence before entering the typed storage preparer.
			for (size_t ordinal = 0U; ordinal < order.size(); ++ordinal) {
				tree.nodes[order[ordinal]].page_id = pageIds[ordinal];
			}

			std::vector<LargeValueManifest> manifests;
			manifests.reserve(order.size());
			for (const size_t nodeIndex: order) {
				const PlannedManifestNode &node = tree.nodes[nodeIndex];
				LargeValueManifest manifest;
				manifest.value_id = valueId;
				manifest.page_id = node.page_id;
				manifest.generation = generation;
				manifest.committed_lsn = checkpointLsn;
				manifest.logical_bytes = logicalBytes;
				manifest.content_hash = contentHash;
				manifest.kind = kind;
				manifest.codec = codec;
				manifest.media_type = mediaType;
				manifest.node_kind = node.kind;
				manifest.node_level = node.level;
				if (node.kind != LargeValueManifestNodeKind::LVMNK_ROOT) {
					manifest.first_chunk_ordinal = static_cast<uint32_t>(node.first_chunk_index);
					manifest.first_uncompressed_offset = chunks[node.first_chunk_index].uncompressed_offset;
					manifest.node_logical_bytes =
							nodeLogicalBytes(planningChunks, node.first_chunk_index, node.chunk_count);
				}
				if (node.kind == LargeValueManifestNodeKind::LVMNK_LEAF ||
					(node.kind == LargeValueManifestNodeKind::LVMNK_ROOT && node.level == 0U)) {
					manifest.chunks.insert(
							manifest.chunks.end(), chunks.begin() + static_cast<std::ptrdiff_t>(node.first_chunk_index),
							chunks.begin() + static_cast<std::ptrdiff_t>(node.first_chunk_index + node.chunk_count));
				} else {
					manifest.children.reserve(node.children.size());
					for (const size_t childIndex: node.children) {
						const PlannedManifestNode &child = tree.nodes[childIndex];
						manifest.children.push_back(
								{child.page_id, static_cast<uint32_t>(child.first_chunk_index),
								 static_cast<uint32_t>(child.chunk_count),
								 chunks[child.first_chunk_index].uncompressed_offset,
								 nodeLogicalBytes(planningChunks, child.first_chunk_index, child.chunk_count)});
					}
				}
				manifests.push_back(std::move(manifest));
			}
			return manifests;
		}

		void validateRange(uint64_t logicalBytes, uint64_t offset, uint64_t length) {
			if (offset > logicalBytes || length > logicalBytes - offset) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue range is outside the immutable value bounds");
			}
			if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
				if (length > std::numeric_limits<size_t>::max()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "LargeValue range cannot be materialized on this platform");
				}
			}
		}

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void validateManifestChild(const LargeValueManifest &parent, const LargeValueManifestChild &range,
								   const LargeValueManifest &child) {
			size_t childChunkCount = 0U;
			if (child.isLeaf()) {
				childChunkCount = child.chunks.size();
			} else {
				for (const LargeValueManifestChild &grandchild: child.children) {
					if (grandchild.chunk_count > std::numeric_limits<size_t>::max() -
														 childChunkCount) { // ZYX_COV_EXCL_LINE: bounded wire entries
																			// cannot overflow 64-bit size_t.
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							 "LargeValue Manifest child chunk count exceeds this platform's address space");
					}
					childChunkCount += grandchild.chunk_count;
				}
			}
			const uint8_t invalidLevel =
					static_cast<uint8_t>(child.node_kind == LargeValueManifestNodeKind::LVMNK_ROOT) |
					static_cast<uint8_t>(parent.node_level == 0U) |
					static_cast<uint8_t>(child.node_level + 1U != parent.node_level);
			if (invalidLevel != 0U) {
				fail(FormatError::FERR_INVALID_STATE, "LargeValue Manifest child level is invalid");
			}
			const uint8_t invalidRange =
					static_cast<uint8_t>(child.first_chunk_ordinal != range.first_chunk_ordinal) |
					static_cast<uint8_t>(childChunkCount != range.chunk_count) |
					static_cast<uint8_t>(child.first_uncompressed_offset != range.uncompressed_offset) |
					static_cast<uint8_t>(child.node_logical_bytes != range.logical_bytes);
			if (invalidRange != 0U) {
				fail(FormatError::FERR_INVALID_STATE, "LargeValue Manifest child range disagrees with its parent");
			}
		}

	} // namespace

	LargeValueStore::LargeValueStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
									 SharedPagePoolOpenMode openMode) :
		allocator_(io, metadata, openMode), metadata_(metadata),
		content_index_pages_(std::make_unique<SlottedPagePool>(io, metadata, openMode)),
		content_index_(std::make_unique<BTreePageStore>(*content_index_pages_, metadata)), io_(std::move(io)) {}

	LargeValueStore::~LargeValueStore() = default;

	BTreePageStore &LargeValueStore::contentIndex() const { return *content_index_; }

	std::vector<std::byte> LargeValueStore::contentIndexKey(const LargeValueRef &reference) const {
		return makeContentIndexKey(reference);
	}

	LargeValueContentIndexPublicationPlan LargeValueStore::planNativeGraphContentIndexMutation(
				const MetadataSnapshotPin &snapshot, std::span<const NativeGraphLargeValueMutation> values,
				PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, bool forceFreshRebuild) const {
		if (values.empty() || !firstSlot.isValid() || !lastSlot.isValid() || firstSlot > lastSlot) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph LargeValue content-index planning requires values and a valid allocation range");
		}
		std::vector<BTreeScalarIndexOperation> operations;
		operations.reserve(values.size());
		std::set<uint64_t> valueIds;
		for (const NativeGraphLargeValueMutation &value: values) {
			if (!value.reference.value_id.isValid() || !valueIds.insert(value.reference.value_id.value()).second) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph LargeValue content-index planning requires unique valid value identities");
			}
			static_cast<void>(LargeValueRefCodec::encode(value.reference));
			operations.push_back({BTreeScalarIndexOperationKind::BSIOP_UPSERT, contentIndexKey(value.reference),
							  value.reference.value_id.value()});
		}
		return makeContentIndexPublicationPlan(contentIndex(), metadata_, snapshot, operations, firstSlot, lastSlot,
															forceFreshRebuild);
	}

	std::optional<LargeValueRef> LargeValueStore::findContentDuplicate(const MetadataSnapshotPin &snapshot,
																	   std::span<const std::byte> bytes,
																	   LargeValueKind kind, std::string_view mediaType,
																	   size_t maximumCandidates) const {
		return findContentDuplicateWithStats(snapshot, bytes, kind, mediaType, maximumCandidates).reference;
	}

	LargeValueDedupLookupResult LargeValueStore::findContentDuplicateWithStats(const MetadataSnapshotPin &snapshot,
																			   std::span<const std::byte> bytes,
																			   LargeValueKind kind,
																			   std::string_view mediaType,
																			   size_t maximumCandidates) const {
		if (!isLargeValueKind(kind) || maximumCandidates == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "LargeValue content-index lookup requires a valid kind and nonzero candidate bound");
		}
		const std::string canonicalMediaType(mediaType);
		validateMediaType(canonicalMediaType);
		if (maximumCandidates == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "LargeValue content-index candidate bound cannot reserve an overflow sentinel");
		}
		const std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> contentHash = utils::Sha256::digest(bytes);
		const std::vector<std::byte> identity =
				contentIdentityPrefix(kind, bytes.size(), contentHash, canonicalMediaType);
		BTreeIndex index(RSS_LARGE_VALUE_CONTENT_INDEX_ID, contentIndex(), metadata_);
		const std::vector<BTreePageEntry> candidates =
				index.scan(snapshot, std::span<const std::byte>(identity), maximumCandidates);
		size_t candidatesSeen = 0U;
		for (const BTreePageEntry &candidate: candidates) {
			if (!startsWith(candidate.key, identity)) {
				break;
			}
			if (candidate.key.size() !=
				identity.size() + sizeof(uint64_t)) { // ZYX_COV_EXCL_LINE: BTree codec rejects malformed packed
													  // content-index keys first.
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue content-index candidate key has an invalid identity suffix");
			}
			++candidatesSeen;
			const uint64_t valueId =
					decodeBigEndianU64(std::span(candidate.key).subspan(identity.size(), sizeof(uint64_t)));
			const uint8_t invalidCandidate =
					static_cast<uint8_t>(valueId == 0U) | static_cast<uint8_t>(candidate.value_or_child != valueId);
			if (invalidCandidate != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue content-index candidate disagrees with its canonical value identity");
			}
			const LargeValueManifest candidateManifest = manifest(snapshot, LargeValueId(valueId));
			const LargeValueRef candidateReference = candidateManifest.reference();
			const uint8_t identityMismatch = static_cast<uint8_t>(candidateReference.logical_bytes != bytes.size()) |
											 static_cast<uint8_t>(candidateReference.content_hash != contentHash) |
											 static_cast<uint8_t>(candidateReference.kind != kind) |
											 static_cast<uint8_t>(candidateReference.media_type != canonicalMediaType);
			const std::vector<std::byte> candidateBytes = read(snapshot, candidateReference);
			const uint8_t invalidContent =
					identityMismatch | static_cast<uint8_t>(candidateBytes.size() != bytes.size()) |
					static_cast<uint8_t>(!std::equal(candidateBytes.begin(), candidateBytes.end(), bytes.begin()));
			if (invalidContent != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue content-index candidate does not reproduce its indexed logical bytes");
			}
			return {candidateReference, candidatesSeen};
		}
		return {std::nullopt, candidatesSeen};
	}

	LargeValueRef LargeValueStore::planReference(LargeValueId valueId, Generation generation,
												 std::span<const std::byte> bytes, LargeValueKind kind,
												 std::string mediaType, const LargeValueCompressionOptions &compression,
												 LargeValueExtentClass extentClass) const {
		const uint8_t invalidIdentity = static_cast<uint8_t>(!valueId.isValid()) |
										static_cast<uint8_t>(!generation.isValid()) |
										static_cast<uint8_t>(!isLargeValueKind(kind));
		if (invalidIdentity != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue reference planning requires valid immutable identity");
		}
		validateMediaType(mediaType);
		validateCompressionOptions(compression);
		LargeValueCodec aggregate = LargeValueCodec::LVC_RAW;
		bool hasChunk = false;
		visitPlannedChunks(bytes, compression, extentClass, [&aggregate, &hasChunk](const StreamedChunkPlan &chunk) {
			if (!hasChunk) {
				aggregate = chunk.codec;
				hasChunk = true;
			} else if (aggregate != chunk.codec) {
				aggregate = LargeValueCodec::LVC_MIXED;
			}
		});
		LargeValueRef reference;
		reference.value_id = valueId;
		reference.generation = generation;
		reference.logical_bytes = bytes.size();
		reference.content_hash = utils::Sha256::digest(bytes);
		reference.kind = kind;
		reference.codec = aggregate;
		reference.media_type = std::move(mediaType);
		static_cast<void>(LargeValueRefCodec::encode(reference));
		return reference;
	}

	size_t LargeValueStore::plannedDataPageCount(std::span<const std::byte> bytes,
												 const LargeValueCompressionOptions &compression,
												 LargeValueExtentClass extentClass) const {
		validateCompressionOptions(compression);
		size_t pageCount = 0U;
		visitPlannedChunks(bytes, compression, extentClass, [&pageCount](const StreamedChunkPlan &chunk) {
			const size_t chunkPageCount = chunk.page_count;
			if (chunkPageCount > std::numeric_limits<size_t>::max() - pageCount) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue data-page count exceeds the platform size bound");
			}
			pageCount += chunkPageCount;
		});
		return pageCount;
	}

	size_t LargeValueStore::plannedManifestPageCount(std::span<const std::byte> bytes,
													 const LargeValueCompressionOptions &compression,
													 LargeValueExtentClass extentClass) const {
		validateCompressionOptions(compression);
		size_t chunkCount = 0U;
		visitPlannedChunks(bytes, compression, extentClass, [&chunkCount](const StreamedChunkPlan &) {
			if (chunkCount == std::numeric_limits<size_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue chunk count exceeds the platform size bound");
			}
			++chunkCount;
		});
		return plannedManifestNodeCount(chunkCount);
	}

	void LargeValueStore::materializeStreamedValue(const LargeValueStreamedMaterializationRequest &request,
														 LogSequenceNumber checkpointLsn,
														 const LargeValueStreamedOwnerResolver &resolveOwner,
														 const LargeValueStreamedFrameVisitor &visit) const {
		if (!request.reference.value_id.isValid() || !request.reference.generation.isValid() ||
			!checkpointLsn.isValid() || !resolveOwner || !visit ||
			(request.data_page_count == 0U && request.first_data_page_id.isValid()) ||
			(request.data_page_count != 0U && !request.first_data_page_id.isValid()) ||
			!request.first_manifest_page_id.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "streamed LargeValue materialization requires bounded identities, page ranges, and typed consumers");
		}
		validateCompressionOptions(request.compression);
		const LargeValueRef expected = planReference(
				request.reference.value_id, request.reference.generation, request.logical_bytes, request.reference.kind,
				request.reference.media_type, request.compression, request.extent_class);
		const size_t expectedDataPages =
				plannedDataPageCount(request.logical_bytes, request.compression, request.extent_class);
		const size_t expectedManifestPages =
				plannedManifestPageCount(request.logical_bytes, request.compression, request.extent_class);
		if (request.reference != expected || request.data_page_count != expectedDataPages ||
			request.manifest_page_count != expectedManifestPages ||
			(request.data_page_count != 0U &&
			 request.data_page_count - 1U > std::numeric_limits<uint64_t>::max() - request.first_data_page_id.value()) ||
			request.manifest_page_count == 0U ||
			request.manifest_page_count - 1U > std::numeric_limits<uint64_t>::max() - request.first_manifest_page_id.value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "streamed LargeValue materialization disagrees with its deterministic immutable plan");
		}

		const auto pageAt = [](PageId first, size_t ordinal) {
			return PageId(first.value() + static_cast<uint64_t>(ordinal));
		};
		const auto ownerFor = [&](PageId pageId, PageOwnerKind kind) {
			const PageOwnerHeader owner = resolveOwner(pageId, kind);
			const uint8_t invalid = static_cast<uint8_t>(owner.page_id != pageId) |
								static_cast<uint8_t>(owner.owner_id != request.reference.value_id.value()) |
								static_cast<uint8_t>(owner.kind != kind) |
								static_cast<uint8_t>(!owner.generation.isValid()) |
								static_cast<uint8_t>(owner.committed_lsn != checkpointLsn);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "streamed LargeValue owner resolver returned an unauthenticated pool frame identity");
			}
			return owner;
		};
		const uint64_t extentBytesForChunks =
				std::min<uint64_t>(extentBytes(request.extent_class), maximumChunkStoredBytes());
		const auto chunkLogicalBytes = [&](size_t ordinal) {
			return chunkRange(ordinal, request.logical_bytes.size(), extentBytesForChunks, request.compression).second;
		};
		const auto chunkLogicalOffset = [&](size_t ordinal) {
			return chunkRange(ordinal, request.logical_bytes.size(), extentBytesForChunks, request.compression).first;
		};

		size_t dataPageOrdinal = 0U;
		size_t observedChunkCount = 0U;
		visitPlannedChunks(request.logical_bytes, request.compression, request.extent_class,
				[&](const StreamedChunkPlan &expectedChunk) {
					const uint64_t logicalOffset = chunkLogicalOffset(observedChunkCount);
					const uint32_t logicalBytes = chunkLogicalBytes(observedChunkCount);
					const std::span<const std::byte> source = request.logical_bytes.subspan(
							static_cast<size_t>(logicalOffset), logicalBytes);
					std::vector<std::byte> stored = selectStoredPayload(source, request.compression);
					const uint32_t storedBytes = static_cast<uint32_t>(stored.size());
					const uint16_t pageCount = pageCountForBytes(storedBytes);
					if (expectedChunk.page_count != pageCount || dataPageOrdinal > request.data_page_count ||
						pageCount > request.data_page_count - dataPageOrdinal) {
						fail(FormatError::FERR_INVALID_STATE,
							 "streamed LargeValue data-page replay diverged from its measured plan");
					}
					for (uint16_t pageOrdinal = 0U; pageOrdinal < pageCount; ++pageOrdinal) {
						const PageId pageId = pageAt(request.first_data_page_id, dataPageOrdinal + pageOrdinal);
						const uint32_t storedOffset =
								static_cast<uint32_t>(pageOrdinal) * RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
						const uint32_t payloadBytes = std::min(
								RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES, storedBytes - storedOffset);
						const LargeValueDataPage data{request.reference.value_id,
												  pageId,
												  request.reference.generation,
												  static_cast<uint32_t>(observedChunkCount),
												  pageOrdinal,
												  pageCount,
												  storedBytes,
												  storedOffset,
												  {stored.begin() + static_cast<std::ptrdiff_t>(storedOffset),
												   stored.begin() + static_cast<std::ptrdiff_t>(storedOffset + payloadBytes)}};
						const PageOwnerHeader owner = ownerFor(pageId, PageOwnerKind::POK_LARGE_VALUE_DATA);
						visit(owner, LargeValueDataFrame::create(owner, data).serialize(), PageOwnerKind::POK_LARGE_VALUE_DATA);
					}
					dataPageOrdinal += pageCount;
					++observedChunkCount;
				});
		if (dataPageOrdinal != request.data_page_count) {
			fail(FormatError::FERR_INVALID_STATE,
				 "streamed LargeValue data-page replay produced the wrong total page count");
		}

		const size_t chunkCount = observedChunkCount;
		const auto roundedDivision = [](size_t value, size_t divisor) {
			return value / divisor + static_cast<size_t>(value % divisor != 0U);
		};
		std::vector<size_t> levelCounts;
		if (chunkCount > RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) {
			levelCounts.push_back(roundedDivision(chunkCount, RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS));
			while (levelCounts.back() > RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN) {
				levelCounts.push_back(roundedDivision(levelCounts.back(), RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN));
			}
		}
		const size_t actualManifestPages =
				levelCounts.empty() ? 1U : 1U + std::accumulate(levelCounts.begin(), levelCounts.end(), size_t{0U});
		if (actualManifestPages != request.manifest_page_count) {
			fail(FormatError::FERR_INVALID_STATE,
				 "streamed LargeValue Manifest hierarchy disagrees with its reserved logical page range");
		}
		const auto levelPageOrdinal = [&levelCounts](size_t level, size_t ordinal) {
			size_t result = 1U;
			for (size_t higher = levelCounts.size(); higher > level + 1U; --higher) {
				result += levelCounts[higher - 1U];
			}
			return result + ordinal;
		};
		const auto nodeFirstChunk = [](const auto &self, size_t level, size_t ordinal) -> size_t {
			if (level == 0U) {
				return ordinal * RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS;
			}
			return self(self, level - 1U, ordinal * RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN);
		};
		const auto nodeChunkCount = [&levelCounts, &nodeFirstChunk, chunkCount](const auto &self, size_t level,
																						  size_t ordinal) -> size_t {
			const size_t first = nodeFirstChunk(nodeFirstChunk, level, ordinal);
			if (level == 0U) {
				return std::min(chunkCount - first, static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS));
			}
			const size_t firstChild = ordinal * RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN;
			const size_t childCount = std::min(levelCounts[level - 1U] - firstChild,
																					static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN));
			const size_t lastChild = firstChild + childCount - 1U;
			const size_t lastFirst = nodeFirstChunk(nodeFirstChunk, level - 1U, lastChild);
			return lastFirst + self(self, level - 1U, lastChild) - first;
		};
		const auto newManifest = [&](PageId pageId, LargeValueManifestNodeKind kind, uint16_t level) {
			LargeValueManifest manifest;
			manifest.value_id = request.reference.value_id;
			manifest.page_id = pageId;
			manifest.generation = request.reference.generation;
			manifest.committed_lsn = checkpointLsn;
			manifest.logical_bytes = request.reference.logical_bytes;
			manifest.content_hash = request.reference.content_hash;
			manifest.kind = request.reference.kind;
			manifest.codec = request.reference.codec;
			manifest.media_type = request.reference.media_type;
			manifest.node_kind = kind;
			manifest.node_level = level;
			return manifest;
		};
		const auto emitManifest = [&](LargeValueManifest manifest) {
			const PageOwnerHeader owner = ownerFor(manifest.page_id, PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
			visit(owner, LargeValueManifestPage::create(owner, manifest).serialize(),
				  PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
		};
		if (levelCounts.empty()) {
			LargeValueManifest root = newManifest(request.first_manifest_page_id, LargeValueManifestNodeKind::LVMNK_ROOT, 0U);
			dataPageOrdinal = 0U;
			visitPlannedChunks(request.logical_bytes, request.compression, request.extent_class,
					[&](const StreamedChunkPlan &chunk) {
						const size_t ordinal = root.chunks.size();
						const uint32_t logicalBytes = chunkLogicalBytes(ordinal);
						const uint64_t logicalOffset = chunkLogicalOffset(ordinal);
						const std::span<const std::byte> source = request.logical_bytes.subspan(
								static_cast<size_t>(logicalOffset), logicalBytes);
						std::vector<std::byte> stored = selectStoredPayload(source, request.compression);
						root.chunks.push_back({static_cast<uint32_t>(ordinal), chunk.codec, logicalOffset, logicalBytes,
										  static_cast<uint32_t>(stored.size()), utils::calculateCrc(source.data(), source.size()),
										  utils::calculateCrc(stored.data(), stored.size()),
										  pageAt(request.first_data_page_id, dataPageOrdinal), chunk.page_count});
						dataPageOrdinal += chunk.page_count;
					});
			emitManifest(std::move(root));
			return;
		}

		const size_t topLevel = levelCounts.size() - 1U;
		LargeValueManifest root = newManifest(request.first_manifest_page_id, LargeValueManifestNodeKind::LVMNK_ROOT,
													 static_cast<uint16_t>(levelCounts.size()));
		for (size_t child = 0U; child < levelCounts[topLevel]; ++child) {
			const size_t first = nodeFirstChunk(nodeFirstChunk, topLevel, child);
			const size_t count = nodeChunkCount(nodeChunkCount, topLevel, child);
			root.children.push_back({pageAt(request.first_manifest_page_id, levelPageOrdinal(topLevel, child)),
								 static_cast<uint32_t>(first), static_cast<uint32_t>(count), chunkLogicalOffset(first),
								 chunkLogicalOffset(first + count - 1U) + chunkLogicalBytes(first + count - 1U) -
										 chunkLogicalOffset(first)});
		}
		emitManifest(std::move(root));
		size_t leafDataPageOrdinal = 0U;
		for (size_t level = topLevel + 1U; level-- > 0U;) {
			for (size_t ordinal = 0U; ordinal < levelCounts[level]; ++ordinal) {
				const PageId pageId = pageAt(request.first_manifest_page_id, levelPageOrdinal(level, ordinal));
				const size_t first = nodeFirstChunk(nodeFirstChunk, level, ordinal);
				const size_t count = nodeChunkCount(nodeChunkCount, level, ordinal);
				LargeValueManifest node = newManifest(
						pageId, level == 0U ? LargeValueManifestNodeKind::LVMNK_LEAF
												: LargeValueManifestNodeKind::LVMNK_INTERNAL,
						static_cast<uint16_t>(level));
				node.first_chunk_ordinal = static_cast<uint32_t>(first);
				node.first_uncompressed_offset = chunkLogicalOffset(first);
				node.node_logical_bytes = chunkLogicalOffset(first + count - 1U) +
									  chunkLogicalBytes(first + count - 1U) - node.first_uncompressed_offset;
				if (level == 0U) {
					for (size_t chunkOrdinal = first; chunkOrdinal < first + count; ++chunkOrdinal) {
						const uint32_t logicalBytes = chunkLogicalBytes(chunkOrdinal);
						const uint64_t logicalOffset = chunkLogicalOffset(chunkOrdinal);
						const std::span<const std::byte> source = request.logical_bytes.subspan(
								static_cast<size_t>(logicalOffset), logicalBytes);
						std::vector<std::byte> stored = selectStoredPayload(source, request.compression);
						const size_t firstDataOrdinal = leafDataPageOrdinal;
						const uint16_t storedPageCount = pageCountForBytes(static_cast<uint32_t>(stored.size()));
						node.chunks.push_back({static_cast<uint32_t>(chunkOrdinal), selectedCodec(source, request.compression, stored),
										  logicalOffset, logicalBytes, static_cast<uint32_t>(stored.size()),
										  utils::calculateCrc(source.data(), source.size()),
										  utils::calculateCrc(stored.data(), stored.size()),
										  pageAt(request.first_data_page_id, firstDataOrdinal), storedPageCount});
						leafDataPageOrdinal += storedPageCount;
					}
				} else {
					const size_t firstChild = ordinal * RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN;
					const size_t childCount = std::min(levelCounts[level - 1U] - firstChild,
																					static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN));
					for (size_t child = firstChild; child < firstChild + childCount; ++child) {
						const size_t childFirst = nodeFirstChunk(nodeFirstChunk, level - 1U, child);
						const size_t childCountChunks = nodeChunkCount(nodeChunkCount, level - 1U, child);
						node.children.push_back({pageAt(request.first_manifest_page_id, levelPageOrdinal(level - 1U, child)),
											 static_cast<uint32_t>(childFirst), static_cast<uint32_t>(childCountChunks),
											 chunkLogicalOffset(childFirst),
											 chunkLogicalOffset(childFirst + childCountChunks - 1U) +
													 chunkLogicalBytes(childFirst + childCountChunks - 1U) -
													 chunkLogicalOffset(childFirst)});
					}
				}
				emitManifest(std::move(node));
			}
		}
	}

	LargeValueWriteResult LargeValueStore::write(std::span<const std::byte> bytes,
												 const LargeValueWriteOptions &options, LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &publishOptions) {
		validateWriteOptions(options, checkpointLsn);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const LargeValueDedupLookupResult dedup = findContentDuplicateWithStats(
					snapshot, bytes, options.kind, options.media_type, options.maximum_dedup_candidates);
			if (dedup.reference.has_value()) {
				const Superblock visible = snapshot.superblock();
				const LargeValueManifest candidateManifest = manifest(snapshot, *dedup.reference);
				snapshot.release();
				return {visible, *dedup.reference, candidateManifest, true, dedup.candidates_scanned};
			}

			const uint64_t nextValueId = snapshot.superblock().next_large_value_id;
			if (nextValueId ==
				0U) { // ZYX_COV_EXCL_LINE: authenticated Superblock allocation never publishes zero identity.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue logical identity space is exhausted");
			}
			const LargeValueRef reference =
					planReference(LargeValueId(nextValueId), Generation(1U), bytes, options.kind, options.media_type,
								  options.compression, options.extent_class);
			const std::vector<std::byte> key = makeContentIndexKey(reference);
			const std::array<BTreeScalarIndexOperation, 1U> operations{{
					{BTreeScalarIndexOperationKind::BSIOP_UPSERT, key, reference.value_id.value()},
			}};
			LargeValueContentIndexPublicationPlan indexPlan = makeContentIndexPublicationPlan(
					contentIndex(), metadata_, snapshot, operations, options.first_data_slot, options.last_data_slot);
			const Generation expectedMetadataGeneration = snapshot.superblock().generation;
			snapshot.release();

			NativeGraphLargeValueMutation mutation;
			mutation.reference = reference;
			mutation.logical_bytes.assign(bytes.begin(), bytes.end());
			mutation.first_data_slot = options.first_data_slot;
			mutation.last_data_slot = options.last_data_slot;
			mutation.first_manifest_slot = options.first_manifest_slot;
			mutation.last_manifest_slot = options.last_manifest_slot;
			mutation.compression = options.compression;
			mutation.extent_class = options.extent_class;
			mutation.planned_data_page_count = plannedDataPageCount(bytes, options.compression, options.extent_class);
			mutation.planned_manifest_page_count =
					plannedManifestPageCount(bytes, options.compression, options.extent_class);
			NativeGraphDerivedStateMutation derivedState;
			derivedState.index_tree_mutations.push_back(std::move(indexPlan.mutation));
			const NativeGraphPageTransactionWithLargeValuesPreparer valuePreparer =
					[this, &mutation,
					 checkpointLsn](std::span<EntityPageReplacementMaterialization> entities,
									std::span<EntityPageAppendBatchMaterialization> appends,
									std::span<EntityPageNewPoolBatchMaterialization> newPools,
									std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
									std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools,
									std::span<LargeValueMaterializationResult> values) {
						// Metadata owns and validates the full typed materialization plan before
						// and after this preparer. This direct writer supplies exactly one value.
						static_cast<void>(entities);
						static_cast<void>(appends);
						static_cast<void>(newPools);
						static_cast<void>(properties);
						static_cast<void>(newPropertyPools);
						prepareNativeGraphMaterialization(mutation, values.front(), checkpointLsn);
					};
			const NativeGraphDerivedStatePreparer indexPreparer =
					[preparer = std::move(indexPlan.preparer),
					 checkpointLsn](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
						// BTreePageStore plans these batches and Metadata validates them around
						// the callback, so this layer only materializes canonical page bytes.
						preparer(batches.front(), checkpointLsn);
					};
			const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
			const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
			const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
			const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
			const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
			const std::array<NativeGraphLargeValueMutation, 1U> values{mutation};
			const Superblock published = metadata_.mutateNativeGraphPagesWithLargeValues(
					noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
					values, derivedState, checkpointLsn, valuePreparer, indexPreparer, {}, {}, {}, {}, {},
					publishOptions, {}, expectedMetadataGeneration);
			MetadataSnapshotPin publishedSnapshot = metadata_.openSnapshot();
			try {
				// A concurrent writer may have committed after this transaction. The
				// returned Superblock identifies this publication; the current snapshot
				// only needs to authenticate that the immutable reference remains readable.
				const LargeValueManifest publishedManifest = manifest(publishedSnapshot, reference);
				publishedSnapshot.release();
				return {published, reference, publishedManifest, false, dedup.candidates_scanned};
			} catch (...) {
				publishedSnapshot.release();
				throw;
			}
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	void LargeValueStore::prepareNativeGraphMaterialization(const NativeGraphLargeValueMutation &mutation,
															LargeValueMaterializationResult &materialization,
															LogSequenceNumber checkpointLsn, const PoolPreparationWriter &writer) {
		// RelocatableMetadataStore creates this exact plan under its writer lock,
		// validates it both before and after this callback, and retains ownership of
		// the allocation identity. This encoder only fills allocator-owned frame
		// locations and validates the durable bytes it creates.
		validateCompressionOptions(mutation.compression);
		static_cast<void>(extentBytes(mutation.extent_class));
		std::vector<PlannedChunk> chunks =
				planChunks(mutation.logical_bytes, mutation.compression, mutation.extent_class);
		const uint8_t invalidMutation =
				static_cast<uint8_t>(mutation.reference.codec != aggregateCodec(chunks)) |
				static_cast<uint8_t>(mutation.planned_data_page_count != totalPlannedDataPages(chunks)) |
				static_cast<uint8_t>(mutation.planned_manifest_page_count != planManifestTree(chunks).nodes.size());
		if (invalidMutation !=
			0U) { // ZYX_COV_EXCL_LINE: metadata creates and validates this private deterministic plan.
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph LargeValue mutation disagrees with deterministic encoding planning");
		}

		size_t chunkOrdinal = 0U;
		uint16_t chunkPageOrdinal = 0U;
		for (LargeValueDataMaterializationPoolBatch &pool: materialization.data_pools) {
			std::vector<SharedPagePoolPreparedAppend> prepared;
			prepared.reserve(pool.page_ids.size());
			for (const PageId pageId: pool.page_ids) {
				PlannedChunk &chunk = chunks[chunkOrdinal];
				if (chunkPageOrdinal == 0U) {
					chunk.first_page_id = pageId;
				}
				const uint32_t storedOffset =
						static_cast<uint32_t>(chunkPageOrdinal) * RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
				const uint32_t payloadBytes = std::min(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES,
													   static_cast<uint32_t>(chunk.stored.size()) - storedOffset);
				const LargeValueDataPage dataPage{
						mutation.reference.value_id,
						pageId,
						mutation.reference.generation,
						static_cast<uint32_t>(chunkOrdinal),
						chunkPageOrdinal,
						chunk.page_count,
						static_cast<uint32_t>(chunk.stored.size()),
						storedOffset,
						{chunk.stored.begin() + static_cast<std::ptrdiff_t>(storedOffset),
						 chunk.stored.begin() + static_cast<std::ptrdiff_t>(storedOffset + payloadBytes)}};
				const PageOwnerHeader owner{
						pageId,		  mutation.reference.value_id.value(), PageOwnerKind::POK_LARGE_VALUE_DATA,
						0U,			  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,	   pool.pool.generation,
						checkpointLsn};
				const LargeValueDataFrame frame = LargeValueDataFrame::create(owner, dataPage);
				prepared.push_back({owner, frame.serialize(), 0U});
				++chunkPageOrdinal;
				if (chunkPageOrdinal == chunk.page_count) {
					++chunkOrdinal;
					chunkPageOrdinal = 0U;
				}
			}
			if (writer) {
				writer(pool.pool, pool.is_new_pool, pool.prepared_prefix_count, prepared, pool.locations);
			} else if (pool.is_new_pool) {
				allocator_.prepareNewPoolPages(pool.pool, prepared, pool.locations, checkpointLsn,
											   pool.prepared_prefix_count);
			} else {
				if (pool.prepared_prefix_count != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "existing LargeValue data pool cannot carry a fresh-pool frame prefix");
				}
				allocator_.prepareAppendPages(pool.pool, prepared, pool.locations, checkpointLsn);
			}
		}

		std::vector<PageId> manifestPageIds;
		for (LargeValueManifestMaterializationPoolBatch &pool: materialization.manifest_pools) {
			manifestPageIds.insert(manifestPageIds.end(), pool.page_ids.begin(), pool.page_ids.end());
		}
		std::vector<LargeValueManifest> manifests =
				buildManifestNodes(mutation.reference.value_id, mutation.reference.generation, checkpointLsn,
								   mutation.reference.logical_bytes, mutation.reference.content_hash,
								   mutation.reference.kind, aggregateCodec(chunks), mutation.reference.media_type,
								   materializedChunks(mutation.logical_bytes, chunks), manifestPageIds);
		size_t manifestOrdinal = 0U;
		for (LargeValueManifestMaterializationPoolBatch &pool: materialization.manifest_pools) {
			std::vector<SharedPagePoolPreparedAppend> prepared;
			prepared.reserve(pool.page_ids.size());
			for (const PageId pageId: pool.page_ids) {
				const PageOwnerHeader owner{
						pageId,		  mutation.reference.value_id.value(), PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
						0U,			  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,	   pool.pool.generation,
						checkpointLsn};
				const LargeValueManifestPage page = LargeValueManifestPage::create(owner, manifests[manifestOrdinal]);
				prepared.push_back({owner, page.serialize(), 0U});
				++manifestOrdinal;
			}
			if (writer) {
				writer(pool.pool, pool.is_new_pool, pool.prepared_prefix_count, prepared, pool.locations);
			} else if (pool.is_new_pool) {
				allocator_.prepareNewPoolPages(pool.pool, prepared, pool.locations, checkpointLsn,
											   pool.prepared_prefix_count);
			} else {
				if (pool.prepared_prefix_count != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "existing LargeValue Manifest pool cannot carry a fresh-pool frame prefix");
				}
				allocator_.prepareAppendPages(pool.pool, prepared, pool.locations, checkpointLsn);
			}
		}
		materialization.manifest_location = materialization.manifest_pools.front().locations.front();
	}

	void LargeValueStore::prepareNativeGraphMaterializations(
			std::span<const NativeGraphLargeValueMutation> mutations,
			std::span<LargeValueMaterializationResult> materializations, LogSequenceNumber checkpointLsn) {
		if (mutations.size() != materializations.size())
			fail(FormatError::FERR_INVALID_STATE, "LargeValue batch preparation shape mismatch");
		std::optional<detail::PoolPreparationBatch> data, manifests;
		const auto flush = [&](auto &pending) {
			if (!pending) return;
			pending->flush([&](const CatalogEntry &pool, auto pages, auto locations, size_t prefix) {
				allocator_.prepareNewPoolPages(pool, pages, locations, checkpointLsn, prefix);
			});
			pending.reset();
		};
		const PoolPreparationWriter writer = [&](const CatalogEntry &pool, bool isNew, size_t prefix,
				std::span<const SharedPagePoolPreparedAppend> pages, std::span<PageLocation> locations) {
			if (pool.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL &&
				pool.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL)
				fail(FormatError::FERR_INVALID_STATE, "LargeValue batch preparation has wrong pool kind");
			auto &pending = pool.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL ? data : manifests;
			if (!isNew) {
				flush(pending);
				if (prefix != 0U) fail(FormatError::FERR_INVALID_STATE, "existing LargeValue pool has a fresh prefix");
				allocator_.prepareAppendPages(pool, pages, locations, checkpointLsn);
				return;
			}
			if (pending && pending->segmentId() != pool.segment_id) flush(pending);
			if (!pending) pending.emplace(pool, prefix);
			pending->append(pool, prefix, pages, locations);
		};
		for (size_t i = 0; i < mutations.size(); ++i)
			prepareNativeGraphMaterialization(mutations[i], materializations[i], checkpointLsn, writer);
		flush(data); flush(manifests);
		// The encoder records logical manifest IDs before pool writes. Scatter the
		// final physical locations only after every buffered pool was prepared.
		for (auto &materialization : materializations)
			materialization.manifest_location = materialization.manifest_pools.front().locations.front();
	}

	LargeValueManifest LargeValueStore::manifest(const MetadataSnapshotPin &snapshot,
												 const LargeValueRef &reference, const std::function<void(size_t)> &bufferAdmission) const {
		return readManifest(snapshot, reference, bufferAdmission);
	}

	std::vector<std::byte> LargeValueStore::read(const MetadataSnapshotPin &snapshot,
												 const LargeValueRef &reference, const std::function<void(size_t)> &bufferAdmission) const {
		std::vector<std::byte> bytes;
		{
			const LargeValueManifest immutableManifest = readManifest(snapshot, reference, bufferAdmission);
			const size_t manifestBytes = checkedReadBufferSum(immutableManifest.media_type.capacity(),
					checkedReadBufferSum(immutableManifest.chunks.capacity() * sizeof(LargeValueChunk),
							immutableManifest.children.capacity() * sizeof(LargeValueManifestChild)));
			const auto admitRead = [&](size_t retained) {
				if (bufferAdmission) bufferAdmission(checkedReadBufferSum(manifestBytes, retained));
			};
			// Reuse the authenticated root and account for it until content verification completes.
			bytes = readRangeFromManifest(snapshot, immutableManifest, 0U, immutableManifest.logical_bytes,
					bufferAdmission ? std::function<void(size_t)>(admitRead) : std::function<void(size_t)>{});
			if (utils::Sha256::digest(bytes) != immutableManifest.content_hash) {
				fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue full-content SHA-256 does not match its manifest");
			}
		}
		if (bufferAdmission) bufferAdmission(bytes.capacity());
		return bytes;
	}

	std::vector<std::byte> LargeValueStore::readRange(const MetadataSnapshotPin &snapshot,
													  const LargeValueRef &reference, uint64_t offset,
													  uint64_t length) const {
		const LargeValueManifest immutableManifest = readManifest(snapshot, reference);
		return readRangeFromManifest(snapshot, immutableManifest, offset, length);
	}

	std::vector<std::byte> LargeValueStore::readRangeFromManifest(const MetadataSnapshotPin &snapshot,
																  const LargeValueManifest &immutableManifest,
																  const uint64_t offset, const uint64_t length, const std::function<void(size_t)> &bufferAdmission) const {
		return readRangeFromManifestImpl(snapshot, immutableManifest, offset, length, bufferAdmission, nullptr);
	}

	std::vector<std::byte> LargeValueStore::readRangeFromManifestCached(const MetadataSnapshotPin &snapshot,
		const LargeValueManifest &manifest, uint64_t offset, uint64_t length, RangeReadCache &cache,
		const std::function<void(size_t)> &bufferAdmission) const {
		try { return readRangeFromManifestImpl(snapshot, manifest, offset, length, bufferAdmission, &cache); }
		catch (...) { cache.clear(); throw; }
	}

	std::vector<std::byte> LargeValueStore::readRangeFromManifestImpl(const MetadataSnapshotPin &snapshot,
		const LargeValueManifest &immutableManifest, uint64_t offset, uint64_t length,
		const std::function<void(size_t)> &bufferAdmission, RangeReadCache *cache) const {
		if (!metadata_.ownsSnapshot(snapshot)) {
			fail(FormatError::FERR_INVALID_STATE, "LargeValue ranged read requires this store's active snapshot");
		}
		if (!immutableManifest.isRoot()) {
			fail(FormatError::FERR_INVALID_HEADER,
					"LargeValue ranged read requires a directory-routed manifest root");
		}
		validateRange(immutableManifest.logical_bytes, offset, length);
		if (cache && (!cache->valid_ || cache->store_ != this || cache->io_.lock() != io_ ||
			cache->snapshot_ != snapshot.superblock() || cache->mutation_epoch_ != io_->dataMutationEpoch() ||
			cache->value_id_ != immutableManifest.value_id || cache->generation_ != immutableManifest.generation ||
			io_->dataMutationEpoch() == std::numeric_limits<uint64_t>::max())) cache->clear();
		const auto admit = [&](size_t bytes) {
			if (bufferAdmission) bufferAdmission(checkedReadBufferSum(bytes, cache ? cache->retainedBytes() : 0U));
		};
		if (length > std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue ranged read exceeds addressable buffer size");
		}
		std::vector<std::byte> result;
		admit(static_cast<size_t>(length));
        if (!bufferAdmission) detail::admitScopedPropertyVectorGrowth(result, static_cast<size_t>(length));
		result.reserve(static_cast<size_t>(length));
		admit(result.capacity());
		if (length == 0U) {
			return result;
		}
		const uint64_t end = offset + length;
		bool firstDataPage = true;
		const auto readNode = [this, &snapshot, &immutableManifest, offset, end, &result,
								   &firstDataPage, &bufferAdmission, &admit, cache](const auto &self, const LargeValueManifest &node, size_t retainedChildren) -> void {
			if (node.isLeaf()) {
				const auto first =
						std::lower_bound(node.chunks.begin(), node.chunks.end(), offset,
										 [](const LargeValueChunk &chunk, uint64_t begin) {
											 return chunk.uncompressed_offset + chunk.uncompressed_bytes <= begin;
										 });
				for (auto chunk = first; chunk != node.chunks.end() && chunk->uncompressed_offset < end; ++chunk) {
					const uint64_t chunkEnd = chunk->uncompressed_offset + chunk->uncompressed_bytes;
					const BufferPoolReadHint firstPageHint = firstDataPage ? BufferPoolReadHint::BPRH_NORMAL
																				 : BufferPoolReadHint::BPRH_SEQUENTIAL;
					utils::OwnedVector<std::byte> ownedChunk;
					std::span<const std::byte> rawChunk;
					if (cache && cache->valid_ && cache->chunk_ == *chunk &&
						cache->mutation_epoch_ == io_->dataMutationEpoch() &&
						cache->raw_.size() == chunk->uncompressed_bytes) {
						admit(checkedReadBufferSum(result.capacity(), retainedChildren));
						rawChunk = cache->raw_;
						debug::PerfTrace::addValue("native_graph.read.large_value_range_chunk_reuse", 1);
					} else {
						// Drop the old buffer before decoding its replacement. Admission
						// therefore charges one raw chunk, not two retained chunks.
						if (cache) cache->clear();
						const uint64_t epoch = io_->dataMutationEpoch();
						const auto admitChunk = [&](size_t bytes) {
                            admit(checkedReadBufferSum(checkedReadBufferSum(result.capacity(), retainedChildren), bytes));
                        };
                        ownedChunk = readRawChunk(snapshot, immutableManifest, *chunk, firstPageHint,
                            bufferAdmission ? std::function<void(size_t)>(std::ref(admitChunk)) : std::function<void(size_t)>{});
						if (cache && ownedChunk.capacity() <= RSS_LARGE_VALUE_EXTENT_128K_BYTES &&
							epoch == io_->dataMutationEpoch() && epoch != std::numeric_limits<uint64_t>::max()) {
							cache->store_ = this; cache->io_ = io_; cache->snapshot_ = snapshot.superblock();
							cache->mutation_epoch_ = epoch; cache->value_id_ = immutableManifest.value_id;
							cache->generation_ = immutableManifest.generation; cache->chunk_ = *chunk;
							cache->raw_ = std::move(ownedChunk); cache->valid_ = true;
							rawChunk = cache->raw_;
						} else rawChunk = ownedChunk;
					}
					firstDataPage = false;
					const uint64_t copyBegin =
							std::max(offset, chunk->uncompressed_offset) - chunk->uncompressed_offset;
					const uint64_t copyEnd = std::min(end, chunkEnd) - chunk->uncompressed_offset;
					result.insert(result.end(), rawChunk.begin() + static_cast<std::ptrdiff_t>(copyBegin),
								  rawChunk.begin() + static_cast<std::ptrdiff_t>(copyEnd));
				}
				return;
			}
			const auto first = std::lower_bound(node.children.begin(), node.children.end(), offset,
												[&](const LargeValueManifestChild &child, uint64_t begin) {
													return child.uncompressed_offset + child.logical_bytes <= begin;
												});
			for (auto childRange = first; childRange != node.children.end() && childRange->uncompressed_offset < end;
				 ++childRange) {
				const LargeValueManifest child = readManifestNode(snapshot, immutableManifest, childRange->page_id,
																				BufferPoolReadHint::BPRH_SEQUENTIAL, [&](size_t bytes) {
					admit(checkedReadBufferSum(checkedReadBufferSum(result.capacity(), retainedChildren), bytes));
				});
				validateManifestChild(node, *childRange, child);
				const size_t childBytes = checkedReadBufferSum(child.media_type.capacity(),
						child.chunks.capacity() * sizeof(LargeValueChunk) +
						child.children.capacity() * sizeof(LargeValueManifestChild));
				self(self, child, checkedReadBufferSum(retainedChildren, childBytes));
			}
		};
		readNode(readNode, immutableManifest, 0U);
		if (result.size() != length) { // ZYX_COV_EXCL_LINE: manifest validation makes the covered ranges contiguous.
			fail(FormatError::FERR_INVALID_STATE, "LargeValue chunk coverage did not satisfy the requested range");
		}
		admit(result.capacity());
		return result;
	}

	LargeValueManifest LargeValueStore::manifest(const MetadataSnapshotPin &snapshot, LargeValueId valueId,
			const std::function<void(size_t)> &bufferAdmission) const {
		const LargeValueManifestLocation route = metadata_.findLargeValueManifest(snapshot, valueId);
		if (bufferAdmission) bufferAdmission(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		const SharedPagePoolContents contents = allocator_.read(snapshot, route.manifest_page_id);
		const uint8_t invalid = static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) |
								static_cast<uint8_t>(contents.pool.owner_id !=
													 RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) |
								static_cast<uint8_t>(contents.location.page_id != route.manifest_page_id) |
								static_cast<uint8_t>(contents.location.generation != route.generation) |
								static_cast<uint8_t>(contents.owner.generation != contents.location.generation);
		if (invalid !=
			0U) { // ZYX_COV_EXCL_LINE: SharedPagePoolAllocator::read already authenticates route, Catalog, and owner.
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue manifest directory route disagrees with its physical page pool");
		}
		const LargeValueManifest immutableManifest =
				LargeValueManifestPage::decodeManifest(contents.owner, contents.payload, [&](size_t bytes) {
					if (bufferAdmission) bufferAdmission(checkedReadBufferSum(contents.payload.capacity(), bytes));
				});
		const uint8_t invalidManifest =
				static_cast<uint8_t>(immutableManifest.value_id != valueId) |
				static_cast<uint8_t>(immutableManifest.page_id != route.manifest_page_id) |
				static_cast<uint8_t>(immutableManifest.node_kind != LargeValueManifestNodeKind::LVMNK_ROOT);
		if (invalidManifest != 0U) {
			fail(FormatError::FERR_INVALID_STATE, "LargeValue manifest does not match its logical directory route");
		}
		return immutableManifest;
	}

	LargeValueManifest LargeValueStore::readManifestNode(const MetadataSnapshotPin &snapshot,
																		  const LargeValueManifest &root, PageId pageId,
																		  BufferPoolReadHint hint, const std::function<void(size_t)> &bufferAdmission) const {
		// manifestTree inserts the directory-routed root into its observed set before
		// reaching here, so this private helper receives only a valid non-root child.
		if (bufferAdmission) bufferAdmission(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId, hint);
		const uint8_t invalid = static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) |
								static_cast<uint8_t>(contents.pool.owner_id !=
													 RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) |
								static_cast<uint8_t>(contents.location.page_id != pageId) |
								static_cast<uint8_t>(contents.owner.generation != contents.location.generation);
		if (invalid != 0U) { // ZYX_COV_EXCL_LINE: SharedPagePoolAllocator::read already authenticates child route,
							 // Catalog, and owner.
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue Manifest node route disagrees with its physical page pool");
		}
		const LargeValueManifest node = LargeValueManifestPage::decodeManifest(contents.owner, contents.payload,
				[&](size_t bytes) {
					if (bufferAdmission) bufferAdmission(checkedReadBufferSum(contents.payload.capacity(), bytes));
				});
		const uint8_t invalidNode = static_cast<uint8_t>(node.page_id != pageId) |
									static_cast<uint8_t>(node.node_kind == LargeValueManifestNodeKind::LVMNK_ROOT) |
									static_cast<uint8_t>(node.reference() != root.reference());
		if (invalidNode != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue Manifest node does not match its root immutable identity");
		}
		return node;
	}

	std::vector<LargeValueManifest> LargeValueStore::manifestTree(const MetadataSnapshotPin &snapshot,
																  const LargeValueManifest &root) const {
		if (!root.isRoot()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "LargeValue Manifest tree traversal requires a directory-routed root");
		}
		std::vector<LargeValueManifest> nodes;
		nodes.push_back(root);
		std::set<uint64_t> observedPageIds;
		observedPageIds.insert(root.page_id.value());
		for (size_t ordinal = 0U; ordinal < nodes.size(); ++ordinal) {
			// Child discovery can grow `nodes`; retain a value copy so reallocation
			// cannot invalidate the parent while its remaining ranges are checked.
			const LargeValueManifest parent = nodes[ordinal];
			for (const LargeValueManifestChild &range: parent.children) {
				if (!observedPageIds.insert(range.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue Manifest tree has a duplicate or cyclic child Page ID");
				}
				const LargeValueManifest child =
						readManifestNode(snapshot, root, range.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL);
				validateManifestChild(parent, range, child);
				nodes.push_back(child);
			}
		}
		return nodes;
	}

	LargeValueManifest LargeValueStore::readManifest(const MetadataSnapshotPin &snapshot,
													 const LargeValueRef &reference, const std::function<void(size_t)> &bufferAdmission) const {
		const LargeValueManifest immutableManifest = manifest(snapshot, reference.value_id, bufferAdmission);
		if (immutableManifest.reference() != reference) {
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue manifest does not match the requested immutable reference");
		}
		return immutableManifest;
	}

	utils::OwnedVector<std::byte> LargeValueStore::readRawChunk(const MetadataSnapshotPin &snapshot,
																		 const LargeValueManifest &manifest,
																		 const LargeValueChunk &chunk,
																		 BufferPoolReadHint firstPageHint, const std::function<void(size_t)> &bufferAdmission) const {
		if constexpr (sizeof(size_t) < sizeof(uint32_t)) {
			if (chunk.stored_bytes > std::numeric_limits<size_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "LargeValue stored chunk exceeds this platform's address space");
			}
		}
        const auto owner = bufferAdmission
            ? std::shared_ptr<std::pmr::memory_resource>(std::shared_ptr<std::pmr::memory_resource>{}, std::pmr::new_delete_resource())
            : utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr;
        utils::OwnedAllocationScope allocationScope(owner);
		utils::OwnedVector<std::byte> bytes;
		if (bufferAdmission) bufferAdmission(chunk.stored_bytes);
		bytes.reserve(chunk.stored_bytes);
		if (bufferAdmission) bufferAdmission(bytes.capacity());
		for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
			const PageId pageId(chunk.first_data_page_id.value() + ordinal);
			const BufferPoolReadHint pageHint =
					ordinal == 0U ? firstPageHint : BufferPoolReadHint::BPRH_SEQUENTIAL;
			if (bufferAdmission) bufferAdmission(checkedReadBufferSum(bytes.capacity(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES));
			const SharedPagePoolContents contents = allocator_.read(snapshot, pageId, pageHint);
			const uint8_t invalid =
					static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL) |
					static_cast<uint8_t>(contents.pool.owner_id != RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID) |
					static_cast<uint8_t>(contents.location.page_id != pageId) |
					static_cast<uint8_t>(contents.owner.generation != contents.location.generation);
			if (invalid != 0U) { // ZYX_COV_EXCL_LINE: SharedPagePoolAllocator::read already authenticates data route,
								 // Catalog, and owner.
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue data-page route disagrees with its immutable manifest");
			}
            detail::PropertyDecodeWorkspace pageWorkspace(owner);
            if (!bufferAdmission) detail::admitScopedPropertyVectorStorage<std::byte>(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES);
			const LargeValueDataPage data = LargeValueDataFrame::decodePage(contents.owner, contents.payload,
					[&](size_t retained) {
						if (bufferAdmission) bufferAdmission(checkedReadBufferSum(
								checkedReadBufferSum(bytes.capacity(), contents.payload.capacity()), retained));
					});
			const uint8_t invalidData = static_cast<uint8_t>(data.value_id != manifest.value_id) |
										static_cast<uint8_t>(data.page_id != pageId) |
										static_cast<uint8_t>(data.generation != manifest.generation) |
										static_cast<uint8_t>(data.chunk_ordinal != chunk.ordinal) |
										static_cast<uint8_t>(data.page_ordinal != ordinal) |
										static_cast<uint8_t>(data.page_count != chunk.data_page_count) |
										static_cast<uint8_t>(data.stored_chunk_bytes != chunk.stored_bytes);
			if (invalidData != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue data page does not match its manifest chunk identity");
			}
			bytes.insert(bytes.end(), data.payload.begin(), data.payload.end());
		}
		const uint8_t invalidStoredChecksum = static_cast<uint8_t>(bytes.size() != chunk.stored_bytes) |
											  static_cast<uint8_t>(checksum(bytes) != chunk.stored_crc);
		if (invalidStoredChecksum != 0U) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH,
				 "LargeValue stored chunk is truncated or fails its integrity checksum");
		}
		utils::OwnedVector<std::byte> raw;
		if (chunk.codec == LargeValueCodec::LVC_RAW) {
			// RAW already owns the uncompressed bytes; transfer instead of cloning.
			raw = std::move(bytes);
			if (bufferAdmission) bufferAdmission(raw.capacity());
		} else {
			if (chunk.uncompressed_bytes > std::numeric_limits<size_t>::max() - bytes.capacity()) fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue decompression buffers overflowed");
			if (bufferAdmission) bufferAdmission(bytes.capacity() + chunk.uncompressed_bytes);
			raw = decompressPayload(bytes, chunk.codec, chunk.uncompressed_bytes);
			if (raw.capacity() > std::numeric_limits<size_t>::max() - bytes.capacity()) fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue retained decompression buffers overflowed");
			if (bufferAdmission) bufferAdmission(bytes.capacity() + raw.capacity());
		}
		const uint8_t invalidUncompressedChecksum = static_cast<uint8_t>(raw.size() != chunk.uncompressed_bytes) |
													static_cast<uint8_t>(checksum(raw) != chunk.uncompressed_crc);
		if (invalidUncompressedChecksum != 0U) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH,
				 "LargeValue uncompressed chunk is truncated or fails its integrity checksum");
		}
		if (bufferAdmission) bufferAdmission(raw.capacity());
		debug::PerfTrace::addValue("native_graph.read.large_value_chunks_read", 1);
		return raw;
	}

	PageRetirement LargeValueStore::planPageRetirement(const MetadataSnapshotPin &snapshot, PageId pageId,
													   LargeValueId valueId, PageOwnerKind ownerKind) const {
		const uint8_t invalidRetirementRequest =
				static_cast<uint8_t>(!pageId.isValid()) | static_cast<uint8_t>(!valueId.isValid()) |
				static_cast<uint8_t>(ownerKind != PageOwnerKind::POK_LARGE_VALUE_DATA &&
									 ownerKind != PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
		if (invalidRetirementRequest != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "LargeValue retirement planning requires a page, value identity, and typed owner kind");
		}
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		const SegmentKind expectedPoolKind = ownerKind == PageOwnerKind::POK_LARGE_VALUE_DATA
													 ? SegmentKind::SGK_LARGE_VALUE_DATA_POOL
													 : SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
		const uint64_t expectedPoolOwnerId = ownerKind == PageOwnerKind::POK_LARGE_VALUE_DATA
												 ? RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID
												 : RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
		const uint8_t invalid = static_cast<uint8_t>(contents.pool.kind != expectedPoolKind) |
								static_cast<uint8_t>(contents.pool.owner_id != expectedPoolOwnerId) |
								static_cast<uint8_t>(contents.location.page_id != pageId) |
								static_cast<uint8_t>(contents.owner.page_id != pageId) |
								static_cast<uint8_t>(contents.owner.owner_id != valueId.value()) |
								static_cast<uint8_t>(contents.owner.kind != ownerKind) |
								static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
								static_cast<uint8_t>(contents.owner.flags != 0U);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue retirement planning found a stale or incompatible physical page owner");
		}
		if (ownerKind == PageOwnerKind::POK_LARGE_VALUE_DATA) {
			// The frame codec independently proves owner/data identity before retirement.
			static_cast<void>(LargeValueDataFrame::open(contents.owner, contents.payload));
		} else {
			// The manifest codec independently proves owner/manifest identity before retirement.
			static_cast<void>(LargeValueManifestPage::open(contents.owner, contents.payload));
		}
		return {contents.location, valueId.value(), ownerKind, contents.owner.committed_lsn};
	}

	SharedPagePoolAllocation LargeValueStore::evacuateManifestExpected(MetadataSnapshotPin &snapshot,
																	   const LargeValueManifestLocation &expected,
																	   SegmentId destinationSegmentId,
																	   LogSequenceNumber checkpointLsn,
																	   const MetadataPublishOptions &publishOptions) {
		try {
			const SharedPagePoolContents source = allocator_.read(snapshot, expected.manifest_page_id);
			const CatalogEntry target = metadata_.findCatalog(snapshot, destinationSegmentId);
			const LargeValueManifestPage page = LargeValueManifestPage::open(source.owner, source.payload);
			const LargeValueManifest &manifest = page.manifest();
			const uint8_t invalid =
					static_cast<uint8_t>(source.pool.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) |
					static_cast<uint8_t>(source.pool.owner_id !=
									 RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) |
					static_cast<uint8_t>(source.owner.page_id != expected.manifest_page_id) |
					static_cast<uint8_t>(source.owner.owner_id != expected.value_id.value()) |
					static_cast<uint8_t>(source.owner.kind != PageOwnerKind::POK_LARGE_VALUE_MANIFEST) |
					static_cast<uint8_t>(source.owner.generation != source.location.generation) |
					static_cast<uint8_t>(source.location.generation != expected.generation) |
					static_cast<uint8_t>(manifest.value_id != expected.value_id) |
					static_cast<uint8_t>(manifest.page_id != expected.manifest_page_id) |
					static_cast<uint8_t>(target.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) |
					static_cast<uint8_t>(target.owner_id != RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) |
					static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(snapshot, target.physical_slot));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue Manifest evacuation requires a current compatible manifest and destination pool");
			}
			PageOwnerHeader destinationOwner = source.owner;
			destinationOwner.generation = target.generation;
			destinationOwner.committed_lsn = checkpointLsn;
			const LargeValueManifestPage relocated = page.rebindOwner(destinationOwner);
			const std::vector<std::byte> payload = relocated.serialize();
			snapshot.release();

			SharedPagePoolEvacuationRequest request;
			request.source_segment_id = source.pool.segment_id;
			request.source_generation = source.pool.generation;
			request.destination_segment_id = destinationSegmentId;
			request.destination_generation = target.generation;
			request.page_id = expected.manifest_page_id;
			request.expected_owner_lsn = source.owner.committed_lsn;
			request.page_owner_id = expected.value_id.value();
			request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
			request.page_free_bytes = 0U;
			request.payload = payload;
			const LargeValueManifestLocation replacement{expected.value_id, expected.manifest_page_id,
														 target.generation};
			return allocator_.evacuateLargeValueManifestPage(request, expected, replacement, checkpointLsn,
															 publishOptions);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	SharedPagePoolAllocation LargeValueStore::evacuateDataExpected(MetadataSnapshotPin &snapshot, PageId pageId,
																   LargeValueId valueId, SegmentId destinationSegmentId,
																   LogSequenceNumber checkpointLsn,
																   const MetadataPublishOptions &publishOptions) {
		try {
			const SharedPagePoolContents source = allocator_.read(snapshot, pageId);
			const CatalogEntry target = metadata_.findCatalog(snapshot, destinationSegmentId);
			const LargeValueDataFrame frame = LargeValueDataFrame::open(source.owner, source.payload);
			const LargeValueDataPage &data = frame.page();
			const LargeValueManifest immutableManifest = manifest(snapshot, valueId);
			bool matchesManifestChunk = false;
			for (const LargeValueManifest &node: manifestTree(snapshot, immutableManifest)) {
				if (!node.isLeaf()) {
					continue;
				}
				for (const LargeValueChunk &chunk: node.chunks) {
					if (chunk.ordinal != data.chunk_ordinal) {
						continue;
					}
					matchesManifestChunk = chunk.data_page_count == data.page_count &&
										   data.page_ordinal < chunk.data_page_count &&
										   chunk.first_data_page_id.value() <=
												   std::numeric_limits<uint64_t>::max() - data.page_ordinal &&
										   chunk.first_data_page_id.value() + data.page_ordinal == pageId.value();
					break;
				}
				if (matchesManifestChunk) {
					break;
				}
			}
			const uint8_t invalid =
					static_cast<uint8_t>(source.pool.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL) |
					static_cast<uint8_t>(source.pool.owner_id != RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID) |
					static_cast<uint8_t>(source.owner.page_id != pageId) |
					static_cast<uint8_t>(source.owner.owner_id != valueId.value()) |
					static_cast<uint8_t>(source.owner.kind != PageOwnerKind::POK_LARGE_VALUE_DATA) |
					static_cast<uint8_t>(source.owner.generation != source.location.generation) |
					static_cast<uint8_t>(data.value_id != valueId) | static_cast<uint8_t>(data.page_id != pageId) |
					static_cast<uint8_t>(data.generation != immutableManifest.generation) |
					static_cast<uint8_t>(!matchesManifestChunk) |
					static_cast<uint8_t>(target.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL) |
					static_cast<uint8_t>(target.owner_id != RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID) |
					static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(snapshot, target.physical_slot));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue data-page evacuation requires a current compatible data frame and destination pool");
			}
			PageOwnerHeader destinationOwner = source.owner;
			destinationOwner.generation = target.generation;
			destinationOwner.committed_lsn = checkpointLsn;
			const LargeValueDataFrame relocated = frame.rebindOwner(destinationOwner);
			const std::vector<std::byte> payload = relocated.serialize();
			snapshot.release();

			SharedPagePoolEvacuationRequest request;
			request.source_segment_id = source.pool.segment_id;
			request.source_generation = source.pool.generation;
			request.destination_segment_id = destinationSegmentId;
			request.destination_generation = target.generation;
			request.page_id = pageId;
			request.expected_owner_lsn = source.owner.committed_lsn;
			request.page_owner_id = valueId.value();
			request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_DATA;
			request.page_free_bytes = 0U;
			request.payload = payload;
			return allocator_.evacuatePage(request, checkpointLsn, publishOptions);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	SharedPagePoolAllocation LargeValueStore::evacuate(LargeValueId valueId, SegmentId destinationSegmentId,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &publishOptions) {
		const uint8_t invalidEvacuation = static_cast<uint8_t>(!valueId.isValid()) |
										  static_cast<uint8_t>(!destinationSegmentId.isValid()) |
										  static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidEvacuation != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "LargeValue Manifest evacuation requires a value identity, destination pool, and checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const LargeValueManifestLocation expected = metadata_.findLargeValueManifest(snapshot, valueId);
		return evacuateManifestExpected(snapshot, expected, destinationSegmentId, checkpointLsn, publishOptions);
	}

	SharedPagePoolAllocation LargeValueStore::evacuatePage(PageId pageId, SegmentId destinationSegmentId,
														   LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &publishOptions) {
		const uint8_t invalidEvacuation = static_cast<uint8_t>(!pageId.isValid()) |
										  static_cast<uint8_t>(!destinationSegmentId.isValid()) |
										  static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidEvacuation != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "LargeValue page evacuation requires a page ID, destination pool, and checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const SharedPagePoolContents source = allocator_.read(snapshot, pageId);
			if (source.owner.kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST) {
				const LargeValueManifestPage page = LargeValueManifestPage::open(source.owner, source.payload);
				const LargeValueManifest &node = page.manifest();
				const LargeValueManifest root = manifest(snapshot, node.value_id);
				const LargeValueManifestLocation expected = metadata_.findLargeValueManifest(snapshot, node.value_id);
				if (expected.generation != source.location.generation &&
					pageId == root.page_id) { // ZYX_COV_EXCL_LINE: paired Page/Manifest Directory publication makes a
											  // current root route atomic.
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue manifest page evacuation found an orphaned or stale directory route");
				}
				if (pageId == root.page_id) {
					return evacuateManifestExpected(snapshot, expected, destinationSegmentId, checkpointLsn,
													publishOptions);
				}
				bool reachable = false;
				for (const LargeValueManifest &candidate: manifestTree(snapshot, root)) {
					reachable = reachable || candidate.page_id == pageId;
				}
				const CatalogEntry target = metadata_.findCatalog(snapshot, destinationSegmentId);
				const uint8_t invalid =
						static_cast<uint8_t>(!reachable) |
						static_cast<uint8_t>(source.pool.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) |
						static_cast<uint8_t>(source.pool.owner_id !=
											 RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) |
						static_cast<uint8_t>(source.owner.page_id != pageId) |
						static_cast<uint8_t>(source.owner.owner_id != node.value_id.value()) |
						static_cast<uint8_t>(source.owner.generation != source.location.generation) |
						static_cast<uint8_t>(target.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) |
						static_cast<uint8_t>(target.owner_id !=
											 RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) |
						static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(snapshot, target.physical_slot));
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue non-root Manifest evacuation found an invalid or unreachable node");
				}
				PageOwnerHeader destinationOwner = source.owner;
				destinationOwner.generation = target.generation;
				destinationOwner.committed_lsn = checkpointLsn;
				const LargeValueManifestPage relocated = page.rebindOwner(destinationOwner);
				const std::vector<std::byte> payload = relocated.serialize();
				snapshot.release();
				SharedPagePoolEvacuationRequest request;
				request.source_segment_id = source.pool.segment_id;
				request.source_generation = source.pool.generation;
				request.destination_segment_id = destinationSegmentId;
				request.destination_generation = target.generation;
				request.page_id = pageId;
				request.expected_owner_lsn = source.owner.committed_lsn;
				request.page_owner_id = node.value_id.value();
				request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
				request.page_free_bytes = 0U;
				request.payload = payload;
				return allocator_.evacuatePage(request, checkpointLsn, publishOptions);
			}
			if (source.owner.kind != PageOwnerKind::POK_LARGE_VALUE_DATA) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue page evacuation requires a typed data or manifest frame");
			}
			const LargeValueDataFrame data = LargeValueDataFrame::open(source.owner, source.payload);
			return evacuateDataExpected(snapshot, pageId, data.page().value_id, destinationSegmentId, checkpointLsn,
										publishOptions);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	void LargeValueStore::prepareRetirements(std::span<const ExistingPoolPageRetirementBatch> batches,
											 LogSequenceNumber checkpointLsn) const {
		// Metadata constructs these batches from an already validated retirement set
		// under its writer lock, then revalidates the prepared frames before publish.
		// This layer only emits the typed page-owner retirement records.
		for (const ExistingPoolPageRetirementBatch &batch: batches) {
			const CatalogEntry &pool = batch.pool;
			std::vector<SharedPagePoolPreparedRetirement> prepared;
			prepared.reserve(batch.retirements.size());
			for (const PageRetirement &retirement: batch.retirements) {
				prepared.push_back({retirement.location, retirement.expected_owner_id, retirement.expected_owner_kind,
									retirement.expected_owner_lsn});
			}
			const std::array<PageLocation, 0U> noReplacements{};
			const std::array<SharedPagePoolPreparedReplacement, 0U> noReplacementPayloads{};
			const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
			std::array<PageLocation, 0U> noReplacementLocations{};
			std::array<PageLocation, 0U> noAppendLocations{};
			allocator_.prepareMutationPages(pool, noReplacements, noReplacementPayloads, noAppends,
											noReplacementLocations, noAppendLocations, checkpointLsn, prepared);
		}
	}

	Superblock LargeValueStore::retireUnreachablePages(Generation expectedMetadataGeneration,
													   std::span<const LargeValueId> erasedValueIds,
													   std::span<const PageRetirement> retirements,
													   PhysicalSlotId firstIndexSlot, PhysicalSlotId lastIndexSlot,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &publishOptions) {
		const uint8_t invalidRetirement =
				static_cast<uint8_t>(!expectedMetadataGeneration.isValid()) |
				static_cast<uint8_t>(erasedValueIds.empty()) | static_cast<uint8_t>(retirements.empty()) |
				static_cast<uint8_t>(!firstIndexSlot.isValid()) | static_cast<uint8_t>(!lastIndexSlot.isValid()) |
				static_cast<uint8_t>(firstIndexSlot > lastIndexSlot) | static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidRetirement != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue retirement requires a generation, manifest identities, "
													   "page retirements, Index range, and checkpoint LSN");
		}
		uint64_t previousValueId = 0U;
		for (const LargeValueId valueId: erasedValueIds) {
			if (!valueId.isValid() || valueId.value() <= previousValueId) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue retirement requires unique ordered immutable value identities");
			}
			previousValueId = valueId.value();
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			if (snapshot.superblock().generation != expectedMetadataGeneration) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue retirement scan generation is stale and must be repeated");
			}
			// Retirement makes a value unreachable by removing its content-index and
			// Manifest-directory routes. Require the complete immutable tree here so
			// a caller cannot leave orphaned child Manifest or data frames behind.
			std::set<std::tuple<uint64_t, uint64_t, PageOwnerKind, LogSequenceNumber>> expectedFrames;
			for (const LargeValueId valueId: erasedValueIds) {
				const LargeValueManifest root = manifest(snapshot, valueId);
				for (const LargeValueManifest &node: manifestTree(snapshot, root)) {
					const PageRetirement manifestRetirement =
							planPageRetirement(snapshot, node.page_id, valueId, PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
					expectedFrames.emplace(manifestRetirement.location.page_id.value(), manifestRetirement.expected_owner_id,
									   manifestRetirement.expected_owner_kind, manifestRetirement.expected_owner_lsn);
					for (const LargeValueChunk &chunk: node.chunks) {
						for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
							const PageRetirement dataRetirement = planPageRetirement(
									snapshot, PageId(chunk.first_data_page_id.value() + ordinal), valueId,
									PageOwnerKind::POK_LARGE_VALUE_DATA);
							expectedFrames.emplace(dataRetirement.location.page_id.value(),
											   dataRetirement.expected_owner_id, dataRetirement.expected_owner_kind,
											   dataRetirement.expected_owner_lsn);
						}
					}
				}
			}
			std::set<std::tuple<uint64_t, uint64_t, PageOwnerKind, LogSequenceNumber>> requestedFrames;
			for (const PageRetirement &retirement: retirements) {
				requestedFrames.emplace(retirement.location.page_id.value(), retirement.expected_owner_id,
										retirement.expected_owner_kind, retirement.expected_owner_lsn);
			}
			if (requestedFrames.size() != retirements.size() || requestedFrames != expectedFrames) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue retirement must include exactly every current Manifest and data frame");
			}
			std::vector<BTreeScalarIndexOperation> operations;
			operations.reserve(erasedValueIds.size());
			for (const LargeValueId valueId: erasedValueIds) {
				const LargeValueManifest valueManifest = manifest(snapshot, valueId);
				operations.push_back({BTreeScalarIndexOperationKind::BSIOP_ERASE,
									  makeContentIndexKey(valueManifest.reference()), 0U});
			}
			if (!metadata_.findIndexRootOptional(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID)
						 .has_value()) { // ZYX_COV_EXCL_LINE: private native publication cannot expose a manifest
										 // without its content index.
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue retirement found a Manifest without the required content index");
			}
			LargeValueContentIndexPublicationPlan indexPlan = makeContentIndexPublicationPlan(
					contentIndex(), metadata_, snapshot, operations, firstIndexSlot, lastIndexSlot);
			snapshot.release();

			NativeGraphDerivedStateMutation derivedState;
			derivedState.index_tree_mutations.push_back(std::move(indexPlan.mutation));
			const NativeGraphPageTransactionWithLargeValuesPreparer noMaterializations =
					[](std::span<EntityPageReplacementMaterialization> entities,
					   std::span<EntityPageAppendBatchMaterialization> appends,
					   std::span<EntityPageNewPoolBatchMaterialization> newPools,
					   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
					   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools,
					   std::span<LargeValueMaterializationResult> values) {
						// Metadata creates empty typed materializations for a retirement-only
						// publication and validates that contract before this callback.
						static_cast<void>(entities);
						static_cast<void>(appends);
						static_cast<void>(newPools);
						static_cast<void>(properties);
						static_cast<void>(newPropertyPools);
						static_cast<void>(values);
					};
			const NativeGraphDerivedStatePreparer indexPreparer =
					[preparer = std::move(indexPlan.preparer),
					 checkpointLsn](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
						preparer(batches.front(), checkpointLsn);
					};
			const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
			const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
			const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
			const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
			const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
			const std::array<NativeGraphLargeValueMutation, 0U> noNewValues{};
			return metadata_.mutateNativeGraphPagesWithLargeValues(
					noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
					noNewValues, derivedState, checkpointLsn, noMaterializations, indexPreparer, {}, {}, retirements,
					erasedValueIds,
					[this, checkpointLsn](std::span<const ExistingPoolPageRetirementBatch> batches) {
						prepareRetirements(batches, checkpointLsn);
					},
					publishOptions, {}, expectedMetadataGeneration);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	void LargeValueStore::reclaimRetiredFrames(SegmentId segmentId) const {
		allocator_.reclaimRetiredFrames(segmentId);
	}

	Superblock LargeValueStore::retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
												LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &publishOptions) {
		return allocator_.retireEmptyPool(segmentId, expectedGeneration, checkpointLsn, publishOptions);
	}

	Superblock LargeValueStore::releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &publishOptions) {
		return allocator_.releaseRetiredEmptyPool(segmentId, expectedGeneration, checkpointLsn, publishOptions);
	}

} // namespace graph::storage
