/**
 * @file NativeBulkEdgePrivateRunMaterializer.cpp
 * @brief Bounded private candidate materialization for streamed Edge runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunMaterializer.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] NativeBulkRunFragmentLocation
		locationFor(const NativeGraphPrivateSegmentMaterialization &identity) {
			if (!identity.isValid()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge private run materializer received an invalid candidate identity");
			}
			return {identity.segment_id, identity.generation, identity.owner_id};
		}

		struct RunPart {
			NativeBulkRunFragmentLocation location;
			uint64_t entry_count = 0U;
			uint64_t encoded_bytes = 0U;
		};

		class LogicalRunManifestBuilder final {
		public:
			LogicalRunManifestBuilder(uint16_t maximumManifestDepth,
									  const NativeBulkEdgePrivateCandidateAllocator &allocateCandidate,
									  const NativeBulkEdgePrivateCandidateWriter &writeCandidate) :
				maximum_manifest_depth_(maximumManifestDepth), allocate_candidate_(allocateCandidate),
				write_candidate_(writeCandidate) {
				if (maximum_manifest_depth_ == 0U) { // ZYX_COV_EXCL_LINE: materializeInitialFragment/materializeOneLevel reject zero depth before construction.
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge private run materializer requires a manifest-depth bound");
				}
				const size_t fanout = manifestFanout();
				// One extra slot holds the location of a root manifest at the
				// maximum permitted encoded level; it is not itself another level.
				levels_.reserve(static_cast<size_t>(maximum_manifest_depth_) + 1U);
				for (uint16_t level = 0U; level <= maximum_manifest_depth_; ++level) {
					levels_.emplace_back();
					levels_.back().reserve(fanout);
				}
			}

			void appendFragment(const NativeBulkRunFragment &fragment) {
				const std::vector<std::byte> payload = NativeBulkRunFragmentCodec::encode(fragment);
				const NativeGraphPrivateSegmentMaterialization identity = allocate_candidate_();
				write_candidate_(identity, payload);
				appendPart(0U, {locationFor(identity), static_cast<uint64_t>(fragment.entries.size()),
								static_cast<uint64_t>(payload.size())});
			}

			[[nodiscard]] NativeBulkRunFragmentLocation finish() {
				const bool hasAnyLevel = std::ranges::any_of(
						levels_.begin(), levels_.end(), [](const std::vector<RunPart> &parts) { return !parts.empty(); });
				if (!hasAnyLevel) { // ZYX_COV_EXCL_LINE: a builder is only finished after appendFragment has populated level zero.
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge private run materializer cannot seal an empty logical run");
				}
				for (;;) {
					size_t lowest = 0U;
					while (lowest < levels_.size() && levels_[lowest].empty()) {
						++lowest;
					}
					if (lowest == levels_.size()) { // ZYX_COV_EXCL_LINE: hasAnyLevel guarantees a populated level remains reachable.
						fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge private run materializer lost its logical-run root");
					}
					const bool hasHigher = std::ranges::any_of(
							levels_.begin() + static_cast<std::ptrdiff_t>(lowest + 1U), levels_.end(),
							[](const std::vector<RunPart> &parts) { return !parts.empty(); });
					if (lowest != 0U && !hasHigher && levels_[lowest].size() == 1U) {
						return levels_[lowest].front().location;
					}
					flushLevel(static_cast<uint16_t>(lowest));
				}
			}

		private:
			[[nodiscard]] static size_t manifestFanout() {
				constexpr size_t payloadBytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
				if (payloadBytes <= RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge private run manifest cannot fit in a candidate segment");
				}
				const size_t fanout = (payloadBytes - RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) /
									  RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES;
				if (fanout < 2U) {
					fail(FormatError::FERR_INVALID_STATE, "native bulk Edge private run manifest fan-out is unusable");
				}
				return fanout;
			}

			void appendPart(uint16_t level, RunPart part) {
				if (level > maximum_manifest_depth_) { // ZYX_COV_EXCL_LINE: only the builder's bounded level transitions call appendPart.
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge private run materializer exceeded its manifest-depth bound");
				}
				std::vector<RunPart> &parts = levels_[level];
				parts.push_back(std::move(part));
				// This final holder is not an encoded level. More than one item would
				// require one more manifest level than the caller admitted.
				if (level == maximum_manifest_depth_) { // ZYX_COV_EXCL_LINE: finish/flushLevel stop before appending a second item to the terminal holder.
					if (parts.size() != 1U) { // ZYX_COV_EXCL_LINE: the terminal level is populated by one bounded flush result at a time.
						fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge private run materializer exceeded its manifest-depth bound");
					}
					return;
				}
				if (parts.size() == manifestFanout()) {
					flushLevel(level);
				}
			}

			void flushLevel(uint16_t level) {
				std::vector<RunPart> parts = std::move(levels_[level]);
				levels_[level].clear();
				if (parts.empty()) { // ZYX_COV_EXCL_LINE: flushLevel is called only for a known nonempty level.
					return;
				}
				NativeBulkRunManifest manifest;
				manifest.level = level;
				auto &references = level == 0U ? manifest.fragments : manifest.children;
				references.reserve(parts.size());
				for (const RunPart &part: parts) {
					if (!part.location.isValid() || part.entry_count == 0U || part.encoded_bytes == 0U || // ZYX_COV_EXCL_LINE: RunPart values originate from validated encoded fragments/manifests.
						part.location.transaction_id != parts.front().location.transaction_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge private run materializer found an invalid run part");
					}
					if (part.entry_count > std::numeric_limits<uint64_t>::max() - manifest.entry_count || // ZYX_COV_EXCL_LINE: encoded fragment and manifest counters are bounded by candidate payload sizes.
						part.encoded_bytes > std::numeric_limits<uint64_t>::max() - manifest.encoded_bytes) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							 "native bulk Edge private run manifest counters overflowed");
					}
					manifest.entry_count += part.entry_count;
					manifest.encoded_bytes += part.encoded_bytes;
					references.push_back(part.location);
				}
				const std::vector<std::byte> payload = NativeBulkRunManifestCodec::encode(manifest);
				const NativeGraphPrivateSegmentMaterialization identity = allocate_candidate_();
				if (identity.owner_id != parts.front().location.transaction_id) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge private run manifest crosses candidate-owner boundaries");
				}
				write_candidate_(identity, payload);
				appendPart(static_cast<uint16_t>(level + 1U),
						   {locationFor(identity), manifest.entry_count, manifest.encoded_bytes});
			}

			uint16_t maximum_manifest_depth_ = 0U;
			const NativeBulkEdgePrivateCandidateAllocator &allocate_candidate_;
			const NativeBulkEdgePrivateCandidateWriter &write_candidate_;
			std::vector<std::vector<RunPart>> levels_;
		};

	} // namespace

	NativeBulkRunFragmentLocation NativeBulkEdgePrivateRunMaterializer::materializeInitialFragment(
			NativeBulkRunFragment fragment, uint16_t maximumManifestDepth,
			const NativeBulkEdgePrivateCandidateAllocator &allocateCandidate,
			const NativeBulkEdgePrivateCandidateWriter &writeCandidate) {
		if (maximumManifestDepth == 0U || !allocateCandidate || !writeCandidate || fragment.entries.empty()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge initial run materialization requires a bounded nonempty fragment");
		}
		LogicalRunManifestBuilder builder(maximumManifestDepth, allocateCandidate, writeCandidate);
		builder.appendFragment(fragment);
		return builder.finish();
	}

	void NativeBulkEdgePrivateRunMaterializer::materializeOneLevel(
			const NativeBulkEdgeRunSource &input, const NativeBulkEdgePrivateRunMaterializationOptions &options,
			const NativeBulkEdgePrivateCandidateAllocator &allocateCandidate,
			const NativeBulkEdgePrivateCandidateWriter &writeCandidate,
			const NativeBulkEdgePrivateRunRootVisitor &visitCompletedRun) {
		if (!input || options.maximum_open_runs < 2U ||
			options.maximum_open_runs == std::numeric_limits<size_t>::max() ||
			options.maximum_fragment_bytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES || options.maximum_entries == 0U ||
			options.maximum_entries == std::numeric_limits<size_t>::max() ||
			options.maximum_manifest_depth == 0U || !allocateCandidate || !writeCandidate || !visitCompletedRun) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge private run materializer requires finite bounds and private callbacks");
		}
		if (options.maximum_fragment_bytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge private run materializer has an invalid candidate-fragment bound");
		}

		std::optional<LogicalRunManifestBuilder> activeRun;
		NativeBulkEdgeRunReducer::reduceOneLevel(
				input, options.maximum_open_runs, options.maximum_fragment_bytes, options.maximum_entries,
				[&](NativeBulkRunFragment &&fragment, bool closesRun) {
					if (!activeRun.has_value()) {
						activeRun.emplace(options.maximum_manifest_depth, allocateCandidate, writeCandidate);
					}
					activeRun->appendFragment(fragment);
					if (closesRun) {
						visitCompletedRun(activeRun->finish());
						activeRun.reset();
					}
				},
				options.duplicate_policy);
		if (activeRun.has_value()) { // ZYX_COV_EXCL_LINE: reduceOneLevel closes every logical run before returning.
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge private run reducer did not close its final logical run");
		}
	}

} // namespace graph::storage
