/**
 * @file NativeBulkEdgeRunReducer.cpp
 * @brief Bounded one-level reduction for streamed Edge-derived runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunReducer.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool keyLess(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		struct MergeItem {
			size_t cursor_ordinal = 0U;
			BTreePageEntry entry;
		};

		struct MergeItemGreater {
			[[nodiscard]] bool operator()(const MergeItem &left, const MergeItem &right) const {
				if (keyLess(right.entry.key, left.entry.key)) {
					return true;
				}
				if (keyLess(left.entry.key, right.entry.key)) {
					return false;
				}
				return left.cursor_ordinal > right.cursor_ordinal;
			}
		};

		[[nodiscard]] size_t entryBytes(const BTreePageEntry &entry) {
			const size_t encodedSize = BTreePageEntryCodec::encodedSize(entry);
			if (encodedSize > std::numeric_limits<size_t>::max() - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge run-reducer entry byte count overflowed");
			}
			return RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + encodedSize;
		}

	} // namespace

	void NativeBulkEdgeRunReducer::reduceOneLevel(const NativeBulkEdgeRunSource &input,
			size_t maximumOpenRuns, size_t maximumFragmentBytes, size_t maximumEntries,
			const NativeBulkEdgeRunFragmentVisitor &visitor, NativeBulkEdgeRunDuplicatePolicy duplicatePolicy) {
		if (!input || maximumOpenRuns < 2U || maximumFragmentBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			maximumEntries == 0U || !visitor ||
			(duplicatePolicy != NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT &&
			 duplicatePolicy != NativeBulkEdgeRunDuplicatePolicy::NBERDP_KEEP_ONE)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge run reducer requires finite fan-in, fragment, entry, source, and visitor bounds");
		}
		NativeBulkEdgeRunCursor cursor = input();
		if (!cursor) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge run reducer source did not open a fragment cursor");
		}

		size_t emittedEntries = 0U;
		bool sawInput = false;
		for (;;) {
			std::vector<OrderedBTreeEntrySource> sources;
			sources.reserve(maximumOpenRuns);
			while (sources.size() < maximumOpenRuns) {
				std::optional<OrderedBTreeEntrySource> run = cursor();
				if (!run.has_value()) {
					break;
				}
				if (!*run) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge run reducer received an empty ordered run source");
				}
				sawInput = true;
				sources.push_back(std::move(*run));
			}
			if (sources.empty()) {
				break;
			}
			std::vector<OrderedBTreeEntryCursor> cursors;
			std::vector<std::optional<std::vector<std::byte>>> previousInputKeys;
			cursors.reserve(sources.size());
			previousInputKeys.resize(sources.size());
			for (const OrderedBTreeEntrySource &sourceFactory: sources) {
				cursors.push_back(sourceFactory());
				if (!cursors.back()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge run reducer fragment source did not open an entry cursor");
				}
			}
			std::vector<MergeItem> ready;
			ready.reserve(cursors.size());
			const auto addNext = [&cursors, &previousInputKeys, &ready](size_t ordinal) {
				std::optional<BTreePageEntry> next = cursors[ordinal]();
				if (!next.has_value()) {
					return;
				}
				if (next->key.empty() ||
					(previousInputKeys[ordinal].has_value() &&
					 !keyLess(*previousInputKeys[ordinal], next->key))) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge run reducer received an unordered input fragment");
				}
				previousInputKeys[ordinal] = next->key;
				ready.push_back({ordinal, std::move(*next)});
				std::push_heap(ready.begin(), ready.end(), MergeItemGreater{});
			};
			for (size_t ordinal = 0U; ordinal < cursors.size(); ++ordinal) {
				addNext(ordinal);
			}
			NativeBulkRunFragment output;
			size_t outputBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
			std::optional<std::vector<std::byte>> previousOutputKey;
			while (!ready.empty()) {
				std::pop_heap(ready.begin(), ready.end(), MergeItemGreater{});
				MergeItem item = std::move(ready.back());
				ready.pop_back();
				const bool duplicate = previousOutputKey.has_value() && *previousOutputKey == item.entry.key;
				if (duplicate && duplicatePolicy == NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge run reducer found duplicate keys in one merge group");
				}
				addNext(item.cursor_ordinal);
				if (duplicate) {
					continue;
				}
				previousOutputKey = item.entry.key;
				const size_t bytes = entryBytes(item.entry);
				if (bytes > maximumFragmentBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge run reducer cannot fit an entry in one candidate fragment");
				}
				if (emittedEntries == maximumEntries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge run reducer exceeded its total entry bound");
				}
				if (outputBytes > maximumFragmentBytes - bytes) {
					visitor(std::move(output), false);
					output = {};
					outputBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
				}
				output.entries.push_back(std::move(item.entry));
				outputBytes += bytes;
				++emittedEntries;
			}
			if (output.entries.empty()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge run reducer merged a nonempty group into no entries");
			}
			visitor(std::move(output), true);
		}
		if (!sawInput) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge run reducer requires at least one sorted input fragment");
		}
	}

} // namespace graph::storage
