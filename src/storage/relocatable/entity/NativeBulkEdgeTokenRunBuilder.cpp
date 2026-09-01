/**
 * @file NativeBulkEdgeTokenRunBuilder.cpp
 * @brief Bounded Catalog-token run construction for streamed Edges.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/entity/NativeBulkEdgeTokenRunBuilder.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] size_t encodedRunEntryBytes(const BTreePageEntry &entry) {
			const size_t encodedSize = BTreePageEntryCodec::encodedSize(entry);
			if (encodedSize > std::numeric_limits<size_t>::max() - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) { // ZYX_COV_EXCL_LINE: encodedSize is produced from a bounded in-memory key and cannot approach size_t overflow.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native bulk Edge token-run entry size overflowed");
			}
			return RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + encodedSize;
		}

		void emitRun(NativeBulkRunFragment &run, size_t maximumFragments, size_t &emittedFragments,
						 const NativeBulkEdgeTokenRunVisitor &visitor) {
			if (run.entries.empty()) {
				return;
			}
			std::ranges::sort(run.entries, {}, &BTreePageEntry::key);
			run.entries.erase(std::unique(run.entries.begin(), run.entries.end(), [](const BTreePageEntry &left,
																	 const BTreePageEntry &right) {
				return left.key == right.key;
			}),
						  run.entries.end());

			constexpr size_t maximumFragmentBytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
			NativeBulkRunFragment fragment;
			size_t fragmentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
			for (BTreePageEntry &entry: run.entries) {
				const size_t entryBytes = encodedRunEntryBytes(entry);
				if (entryBytes > maximumFragmentBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) { // ZYX_COV_EXCL_LINE: token names are bounded by the catalog name limit, far below a segment payload.
					fail(FormatError::FERR_INVALID_HEADER,
							 "native bulk Edge token name cannot fit in one private scratch segment");
				}
				if (fragmentBytes > maximumFragmentBytes - entryBytes) {
					if (emittedFragments == maximumFragments) { // ZYX_COV_EXCL_LINE: buildRuns bounds each run below one segment, so emitRun never needs an internal split.
						fail(FormatError::FERR_INVALID_STATE,
								 "native bulk Edge token-run builder exceeds its candidate fragment bound");
					}
					visitor(std::move(fragment));
					++emittedFragments;
					fragment = {};
					fragmentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
				}
				fragment.entries.push_back(std::move(entry));
				fragmentBytes += entryBytes;
			}
			if (emittedFragments == maximumFragments) {
				fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge token-run builder exceeds its candidate fragment bound");
			}
			visitor(std::move(fragment));
			++emittedFragments;
			run = {};
		}

	} // namespace

	std::vector<std::byte> NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind kind,
																		 std::string_view name) {
		if ((kind != NativeBulkEdgeTokenKind::NBETK_PROPERTY &&
			 kind != NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE) ||
			name.empty() || name.size() > RSS_SYSTEM_CATALOG_NAME_MAX_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge token run has an invalid token name");
		}
		std::vector<std::byte> key;
		key.reserve(1U + name.size());
		key.push_back(static_cast<std::byte>(kind));
		key.insert(key.end(), reinterpret_cast<const std::byte *>(name.data()),
					   reinterpret_cast<const std::byte *>(name.data() + name.size()));
		return key;
	}

	void NativeBulkEdgeTokenRunBuilder::buildRuns(const NativeBulkEdgeRecordSource &source, size_t maximumRecords,
															 size_t maximumRunBytes, size_t maximumFragments,
															 const NativeBulkEdgeTokenRunVisitor &visitor) {
		if (!source || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max() ||
			maximumRunBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			maximumRunBytes == std::numeric_limits<size_t>::max() || maximumFragments == 0U ||
			maximumFragments == std::numeric_limits<size_t>::max() || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge token-run builder requires finite bounds, a source, and a visitor");
		}
		NativeBulkRunFragment current;
		size_t currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
		size_t recordCount = 0U;
		size_t emittedFragments = 0U;
		uint64_t previousEdgeId = 0U;
		const auto append = [&](NativeBulkEdgeTokenKind kind, std::string_view name) {
			BTreePageEntry entry{encodeKey(kind, name), 0U};
			const size_t entryBytes = encodedRunEntryBytes(entry);
			if (entryBytes > maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge token name cannot fit in its configured private run");
			}
			if (currentBytes > maximumRunBytes - entryBytes) {
				emitRun(current, maximumFragments, emittedFragments, visitor);
				currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
			}
			current.entries.push_back(std::move(entry));
			currentBytes += entryBytes;
		};

		NativeBulkEdgeRecordCursor cursor = source();
		while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
			static_cast<void>(NativeBulkEdgeRecordCodec::encodedSize(*record));
			if (++recordCount > maximumRecords || record->edge_id <= previousEdgeId) {
				fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge token-run builder found unordered or over-bound scratch records");
			}
			previousEdgeId = record->edge_id;
			if (!record->type.empty()) {
				append(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, record->type);
			}
			for (const LogicalGraphWriteProperty &property: record->properties) {
				append(NativeBulkEdgeTokenKind::NBETK_PROPERTY, property.key);
			}
		}
		if (recordCount == 0U) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge token-run builder requires a nonempty sealed source");
		}
		emitRun(current, maximumFragments, emittedFragments, visitor);
	}

} // namespace graph::storage
