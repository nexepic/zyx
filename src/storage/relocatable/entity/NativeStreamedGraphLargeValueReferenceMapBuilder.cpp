/**
 * @file NativeStreamedGraphLargeValueReferenceMapBuilder.cpp
 * @brief Bounded provisional-to-canonical LargeValue mapping runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueReferenceMapBuilder.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] std::vector<std::byte> encodeBigEndianU64(uint64_t value) {
			std::vector<std::byte> result(sizeof(value));
			for (size_t index = 0U; index < result.size(); ++index) {
				result[index] = static_cast<std::byte>(value >> ((result.size() - 1U - index) * 8U));
			}
			return result;
		}

		[[nodiscard]] uint64_t decodeBigEndianU64(std::span<const std::byte> bytes) {
			if (bytes.size() != sizeof(uint64_t)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue content entry has an invalid value-ID suffix");
			}
			uint64_t result = 0U;
			for (const std::byte byte: bytes) {
				result = (result << 8U) | static_cast<uint64_t>(std::to_integer<uint8_t>(byte));
			}
			return result;
		}

		[[nodiscard]] size_t encodedRunEntryBytes(const BTreePageEntry &entry) {
			const size_t encodedSize = BTreePageEntryCodec::encodedSize(entry);
			if (encodedSize > std::numeric_limits<size_t>::max() - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native streamed Edge LargeValue reference-map entry size overflowed");
			}
			return RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + encodedSize;
		}

		[[nodiscard]] size_t entriesPerRun(size_t maximumRunBytes) {
			const size_t entryBytes = encodedRunEntryBytes({encodeBigEndianU64(1U), 1U});
			if (maximumRunBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
				entryBytes > maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed Edge LargeValue reference-map entry cannot fit in a private run");
			}
			return (maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) / entryBytes;
		}

		void emitRun(NativeBulkRunFragment &run, size_t maximumFragments, size_t &emittedFragments,
					 const NativeStreamedGraphLargeValueReferenceMapRunVisitor &visitor) {
			if (run.entries.empty()) {
				return;
			}
			std::ranges::sort(run.entries, {}, &BTreePageEntry::key);
			if (std::ranges::adjacent_find(run.entries, {}, &BTreePageEntry::key) != run.entries.end()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue reference-map run has duplicate provisional identities");
			}
			if (emittedFragments == maximumFragments) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue reference-map run exceeds its candidate fragment bound");
			}
			visitor(std::move(run));
			++emittedFragments;
			run = {};
		}

	} // namespace

	NativeStreamedGraphLargeValueReferenceMapRunPlan
	NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
			size_t valueCount, size_t maximumRunBytes, size_t maximumOpenRuns) {
		if (maximumOpenRuns < 2U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue reference-map plan requires a usable merge fan-in");
		}
		if (valueCount == 0U) {
			return {};
		}
		const size_t perRun = entriesPerRun(maximumRunBytes);
		if (valueCount - 1U > std::numeric_limits<size_t>::max() - perRun) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native streamed Edge LargeValue reference-map fragment count overflowed");
		}
		NativeStreamedGraphLargeValueReferenceMapRunPlan result;
		result.initial_fragment_count = (valueCount - 1U) / perRun + 1U;
		result.candidate_segment_count = NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(
				result.initial_fragment_count, maximumOpenRuns);
		return result;
	}

	void NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
			const OrderedBTreeEntrySource &contentEntries, const LargeValueStore &store, LargeValueId firstValueId,
			size_t referenceCount, size_t maximumRunBytes, size_t maximumFragments,
			const NativeStreamedGraphLargeValueReferenceMapRunVisitor &visitor) {
		if (!contentEntries || !firstValueId.isValid() || referenceCount == 0U ||
			maximumRunBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			maximumRunBytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES || maximumFragments == 0U || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue reference-map runs require finite bounded inputs and a consumer");
		}
		LargeValueRef representative;
		representative.value_id = LargeValueId(1U);
		representative.kind = LargeValueKind::LVK_BYTES;
		representative.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
		const std::vector<std::byte> representativeKey = store.contentIndexKey(representative);
		if (representativeKey.size() <= sizeof(uint64_t)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge LargeValue content key has no immutable identity prefix");
		}
		const size_t identityBytes = representativeKey.size() - sizeof(uint64_t);
		NativeBulkRunFragment current;
		size_t currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
		size_t emittedFragments = 0U;
		size_t observedEntries = 0U;
		std::vector<std::byte> previousIdentity;
		LargeValueId canonical{};
		const OrderedBTreeEntryCursor cursor = contentEntries();
		if (!cursor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue reference-map source did not open a cursor");
		}
		for (std::optional<BTreePageEntry> entry = cursor(); entry.has_value(); entry = cursor()) {
			if (entry->key.size() != representativeKey.size() || entry->value_or_child == 0U ||
				observedEntries == referenceCount) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue reference-map content source has an invalid entry count or shape");
			}
			const uint64_t candidateId = decodeBigEndianU64(
					std::span(entry->key).subspan(identityBytes, sizeof(uint64_t)));
			if (candidateId != entry->value_or_child || candidateId < firstValueId.value() ||
				candidateId - firstValueId.value() >= referenceCount) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue reference-map content source has an invalid provisional identity");
			}
			const std::vector<std::byte> identity(entry->key.begin(), entry->key.begin() + static_cast<ptrdiff_t>(identityBytes));
			if (previousIdentity != identity) {
				previousIdentity = identity;
				canonical = LargeValueId(candidateId);
			}
			const BTreePageEntry mapped{encodeBigEndianU64(candidateId), canonical.value()};
			const size_t entryBytes = encodedRunEntryBytes(mapped);
			if (entryBytes > maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed Edge LargeValue reference-map entry cannot fit in a private run");
			}
			if (currentBytes > maximumRunBytes - entryBytes) {
				emitRun(current, maximumFragments, emittedFragments, visitor);
				currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
			}
			current.entries.push_back(mapped);
			currentBytes += entryBytes;
			++observedEntries;
		}
		emitRun(current, maximumFragments, emittedFragments, visitor);
		if (observedEntries != referenceCount) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge LargeValue reference-map source omitted a provisional identity");
		}
	}

} // namespace graph::storage
