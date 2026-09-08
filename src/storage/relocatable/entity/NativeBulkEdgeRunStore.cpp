/**
 * @file NativeBulkEdgeRunStore.cpp
 * @brief Bounded hierarchical scratch manifests for streamed native Edges.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <set>

#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t FLAGS_OFFSET = 8U;
		constexpr size_t LEVEL_OFFSET = 10U;
		constexpr size_t REFERENCE_COUNT_OFFSET = 12U;
		constexpr size_t RECORD_COUNT_OFFSET = 16U;
		constexpr size_t ENCODED_BYTES_OFFSET = 24U;
		constexpr size_t FIRST_EDGE_ID_OFFSET = 32U;
		constexpr size_t LAST_EDGE_ID_OFFSET = 40U;
		constexpr size_t TRANSACTION_ID_OFFSET = 48U;
		constexpr size_t REFERENCES_CRC_OFFSET = 56U;
		constexpr size_t HEADER_CRC_OFFSET = 60U;
		constexpr size_t RESERVED0_OFFSET = 64U;
		constexpr size_t RESERVED1_OFFSET = 72U;
		constexpr size_t RESERVED2_OFFSET = 80U;
		constexpr size_t RESERVED3_OFFSET = 88U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			const std::byte empty{};
			return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size()); // ZYX_COV_EXCL_LINE: persisted run and manifest payloads always contain framing bytes; empty is a defensive CRC fallback.
		}

		[[nodiscard]] size_t checkedMultiply(size_t left, size_t right, const char *message) {
			if (left != 0U && right > std::numeric_limits<size_t>::max() / left) { // ZYX_COV_EXCL_LINE: reference counts are read from bounded payloads before multiplication.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return left * right;
		}

		[[nodiscard]] size_t checkedAdd(size_t left, size_t right, const char *message) {
			if (right > std::numeric_limits<size_t>::max() - left) { // ZYX_COV_EXCL_LINE: manifest framing and bounded references cannot overflow host size_t.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return left + right;
		}

		[[nodiscard]] const std::vector<NativeBulkRunFragmentLocation> &
		references(const NativeBulkEdgeRunManifest &manifest) {
			if (manifest.level == 0U) {
				return manifest.runs;
			}
			return manifest.children;
		}

		void validateManifestShape(const NativeBulkEdgeRunManifest &manifest, bool encoding) {
			const std::vector<NativeBulkRunFragmentLocation> &items = references(manifest);
			if ((encoding && manifest.location.isValid()) || (!encoding && !manifest.location.isValid()) ||
				items.empty() || items.size() > std::numeric_limits<uint32_t>::max() ||
				manifest.record_count == 0U || manifest.encoded_bytes == 0U || manifest.first_edge_id == 0U ||
				manifest.last_edge_id < manifest.first_edge_id) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest has invalid dimensions");
			}
			for (const NativeBulkRunFragmentLocation &item: items) {
				if (!item.isValid() || item.transaction_id != (encoding ? items.front().transaction_id : manifest.location.transaction_id) ||
					(!encoding && item.segment_id == manifest.location.segment_id)) {
					fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest has invalid private references");
				}
			}
		}

		class NativeBulkEdgeManifestCursor final {
		public:
			NativeBulkEdgeManifestCursor(NativeBulkRunFragmentLocation root, uint16_t maximumDepth,
										 NativeBulkRunPayloadReader reader) :
					maximum_depth_(maximumDepth), reader_(std::move(reader)) {
				pushManifest(root, std::nullopt);
			}

			[[nodiscard]] std::optional<NativeBulkEdgeRecord> next() {
				for (;;) {
					if (run_.has_value() && run_offset_ < run_->records.size()) {
						NativeBulkEdgeRecord record = std::move(run_->records[run_offset_++]);
						if ((last_edge_id_ != 0U && record.edge_id <= last_edge_id_) ||
							emitted_ == root_record_count_) {
							fail(FormatError::FERR_INVALID_STATE,
									 "native bulk Edge source lost strict global record ordering");
						}
						last_edge_id_ = record.edge_id;
						++emitted_;
						return record;
					}
					run_.reset();
					run_offset_ = 0U;
					if (stack_.empty()) {
						if (emitted_ != root_record_count_) {
							fail(FormatError::FERR_INVALID_STATE,
									 "native bulk Edge source record count does not match its root manifest");
						}
						return std::nullopt;
					}
					Frame &frame = stack_.back();
					const std::vector<NativeBulkRunFragmentLocation> &references =
							frame.manifest.level == 0U ? frame.manifest.runs : frame.manifest.children;
					if (frame.next_reference == references.size()) {
						const Aggregate aggregate = finishFrame(frame);
						stack_.pop_back();
						if (!stack_.empty()) {
							appendAggregate(stack_.back(), aggregate);
						}
						continue;
					}
					const NativeBulkRunFragmentLocation nextLocation = references[frame.next_reference++];
					if (frame.manifest.level == 0U) {
						run_ = NativeBulkEdgeRunCodec::decode(reader_(nextLocation));
						if (run_->records.empty() || run_->records.front().edge_id <= last_edge_id_) {
							fail(FormatError::FERR_INVALID_STATE,
									 "native bulk Edge source run ordering is inconsistent with its manifest");
						}
						appendRun(frame, *run_);
					} else {
						pushManifest(nextLocation, static_cast<uint16_t>(frame.manifest.level - 1U));
					}
				}
			}

		private:
			struct Aggregate {
				uint64_t record_count = 0U;
				uint64_t encoded_bytes = 0U;
				uint64_t first_edge_id = 0U;
				uint64_t last_edge_id = 0U;
			};

			struct Frame {
				NativeBulkEdgeRunManifest manifest;
				size_t next_reference = 0U;
				Aggregate aggregate;
			};

			static void appendAggregate(Frame &frame, const Aggregate &aggregate) {
				if (aggregate.record_count == 0U || aggregate.encoded_bytes == 0U || aggregate.first_edge_id == 0U ||
					aggregate.last_edge_id < aggregate.first_edge_id ||
					(frame.aggregate.last_edge_id != 0U && aggregate.first_edge_id <= frame.aggregate.last_edge_id) ||
					frame.aggregate.record_count > std::numeric_limits<uint64_t>::max() - aggregate.record_count ||
					frame.aggregate.encoded_bytes > std::numeric_limits<uint64_t>::max() - aggregate.encoded_bytes) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge manifest aggregate is unordered or overflows its declared bounds");
				}
				if (frame.aggregate.first_edge_id == 0U) {
					frame.aggregate.first_edge_id = aggregate.first_edge_id;
				}
				frame.aggregate.last_edge_id = aggregate.last_edge_id;
				frame.aggregate.record_count += aggregate.record_count;
				frame.aggregate.encoded_bytes += aggregate.encoded_bytes;
			}

			static void appendRun(Frame &frame, const NativeBulkEdgeRun &run) {
				const Aggregate aggregate{static_cast<uint64_t>(run.records.size()),
											 static_cast<uint64_t>(NativeBulkEdgeRunCodec::encodedSize(run)),
											 run.records.front().edge_id, run.records.back().edge_id};
				appendAggregate(frame, aggregate);
			}

			static Aggregate finishFrame(const Frame &frame) {
				const Aggregate &aggregate = frame.aggregate;
				if (aggregate.record_count != frame.manifest.record_count ||
					aggregate.encoded_bytes != frame.manifest.encoded_bytes ||
					aggregate.first_edge_id != frame.manifest.first_edge_id ||
					aggregate.last_edge_id != frame.manifest.last_edge_id) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge manifest aggregate diverges from its sealed descendants");
				}
				return aggregate;
			}

			void pushManifest(NativeBulkRunFragmentLocation location, std::optional<uint16_t> expectedLevel) {
				if (location.transaction_id != owner_id_) {
					if (owner_id_ == 0U && stack_.empty()) {
						owner_id_ = location.transaction_id;
					} else {
						fail(FormatError::FERR_INVALID_STATE, "native bulk Edge source encountered a foreign owner");
					}
				}
				const auto identity = std::pair{location.segment_id.value(), location.generation.value()};
				if (!visited_manifests_.insert(identity).second) {
					fail(FormatError::FERR_INVALID_STATE, "native bulk Edge source found a cyclic manifest hierarchy");
				}
				NativeBulkEdgeRunManifest manifest = NativeBulkEdgeRunManifestCodec::decode(reader_(location), location);
				if (manifest.level >= maximum_depth_ || (expectedLevel.has_value() && manifest.level != *expectedLevel)) {
					fail(FormatError::FERR_INVALID_STATE, "native bulk Edge source has an invalid manifest depth or child level");
				}
				if (stack_.empty()) {
					root_record_count_ = manifest.record_count;
				}
				stack_.push_back({std::move(manifest), 0U, {}});
			}

			uint16_t maximum_depth_ = 0U;
			NativeBulkRunPayloadReader reader_;
			uint64_t owner_id_ = 0U;
			uint64_t root_record_count_ = 0U;
			uint64_t emitted_ = 0U;
			uint64_t last_edge_id_ = 0U;
			std::set<std::pair<uint64_t, uint64_t>> visited_manifests_;
			std::vector<Frame> stack_;
			std::optional<NativeBulkEdgeRun> run_;
			size_t run_offset_ = 0U;
		};

	} // namespace

	NativeBulkEdgeRecordSource nativeBulkEdgeRecordSource(NativeBulkRunFragmentLocation manifestLocation,
			uint16_t maximumManifestDepth, NativeBulkRunPayloadReader payloadReader) {
		if (!manifestLocation.isValid() || maximumManifestDepth == 0U || !payloadReader) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge source requires a valid manifest, depth, and payload reader");
		}
		return [manifestLocation, maximumManifestDepth, payloadReader = std::move(payloadReader)] {
			auto cursor = std::make_shared<NativeBulkEdgeManifestCursor>(manifestLocation, maximumManifestDepth, payloadReader);
			return [cursor] { return cursor->next(); };
		};
	}

	NativeBulkEdgeRecordSource nativeBulkEdgeRecordSource(
			NativeBulkRunStore &store, NativeBulkRunFragmentLocation manifestLocation,
			uint16_t maximumManifestDepth) {
		if (!manifestLocation.isValid() || maximumManifestDepth == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge durable source requires a valid manifest and finite depth");
		}
		return [&store, manifestLocation, maximumManifestDepth] {
			return nativeBulkEdgeRecordSource(
					manifestLocation, maximumManifestDepth, store.openOpaquePayloadReader())();
		};
	}

	NativeBulkEdgeRecordSource nativeBulkEdgeRecordSourceFromPrivateReader(
			const NativeGraphPrivateSegmentSet &privateSegments,
			NativeGraphPrivateSegmentMaterialization manifestIdentity, uint16_t maximumManifestDepth,
			const NativeGraphPrivateSegmentReader &privateSegmentReader) {
		if (!manifestIdentity.isValid() || maximumManifestDepth == 0U || !privateSegmentReader) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge private source requires a manifest identity, finite depth, and scoped reader");
		}
		try {
			static_cast<void>(privateSegments.ordinalOf(manifestIdentity));
		} catch (const FormatException &) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge private source manifest is outside its writer-scoped candidate set");
		}
		auto scopedSegments = std::make_shared<NativeGraphPrivateSegmentSet>(privateSegments);
		const NativeBulkRunFragmentLocation manifestLocation{
				manifestIdentity.segment_id, manifestIdentity.generation, manifestIdentity.owner_id};
		const NativeBulkRunPayloadReader payloadReader =
				[scopedSegments = std::move(scopedSegments), privateSegmentReader](
						const NativeBulkRunFragmentLocation &location) {
					const NativeGraphPrivateSegmentMaterialization identity{
							location.segment_id, location.generation, location.transaction_id};
					try {
						static_cast<void>(scopedSegments->ordinalOf(identity));
					} catch (const FormatException &) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge private source encountered a foreign scratch reference");
					}
					return privateSegmentReader(identity);
				};
		return nativeBulkEdgeRecordSource(manifestLocation, maximumManifestDepth, payloadReader);
	}

	void releaseNativeBulkEdgeManifest(NativeBulkRunStore &store, NativeBulkRunFragmentLocation manifestLocation,
			LogSequenceNumber checkpointLsn) {
		if (!manifestLocation.isValid() || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest release requires an owned root and checkpoint");
		}
		struct PendingManifest {
			NativeBulkRunFragmentLocation location;
			std::optional<uint16_t> expected_level;
		};
		MetadataSnapshotPin snapshot = store.metadata_.openSnapshot();
		std::vector<PendingManifest> pending{{manifestLocation, std::nullopt}};
		std::vector<NativeBulkRunFragmentLocation> locations;
		// One scratch segment may be released only once.  Keeping a unified set
		// also rejects a malformed route that aliases a run with a manifest.
		std::set<std::pair<uint64_t, uint64_t>> seen_locations;
		while (!pending.empty()) {
			const PendingManifest pending_manifest = pending.back();
			pending.pop_back();
			const NativeBulkRunFragmentLocation current = pending_manifest.location;
			if (!seen_locations.insert({current.segment_id.value(), current.generation.value()}).second) {
				snapshot.release();
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge manifest release found a cyclic route");
			}
			const NativeBulkEdgeRunManifest manifest =
					NativeBulkEdgeRunManifestCodec::decode(store.readOpaquePayload(snapshot, current), current);
			if (manifest.level >= store.maximumManifestDepth() ||
				(pending_manifest.expected_level.has_value() && manifest.level != *pending_manifest.expected_level)) {
				snapshot.release();
				fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge manifest release found an unbounded or invalid level hierarchy");
			}
			locations.push_back(current);
			const auto &references = manifest.level == 0U ? manifest.runs : manifest.children;
			for (const NativeBulkRunFragmentLocation &reference: references) {
				if (manifest.level == 0U) {
					if (!seen_locations.insert({reference.segment_id.value(), reference.generation.value()}).second) {
						snapshot.release();
						fail(FormatError::FERR_INVALID_STATE,
								 "native bulk Edge manifest release found a duplicate or aliased run");
					}
					static_cast<void>(NativeBulkEdgeRunCodec::decode(store.readOpaquePayload(snapshot, reference)));
					locations.push_back(reference);
				} else {
					pending.push_back({reference, static_cast<uint16_t>(manifest.level - 1U)});
				}
			}
		}
		snapshot.release();
		// Validate every release-batch LSN before changing allocator state. A
		// terminal LSN on a later batch must not leave the manifest partly retired.
		LogSequenceNumber finalCheckpointLsn = checkpointLsn;
		for (size_t first = store.maximumBatchFragments(); first < locations.size();
			 first += store.maximumBatchFragments()) {
			if (finalCheckpointLsn.value() > std::numeric_limits<uint64_t>::max() - 2U) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native bulk Edge manifest release checkpoint overflowed");
			}
			finalCheckpointLsn = LogSequenceNumber(finalCheckpointLsn.value() + 2U);
		}
		if (finalCheckpointLsn.value() == std::numeric_limits<uint64_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge manifest release checkpoint overflowed");
		}
		for (size_t first = 0U; first < locations.size(); first += store.maximumBatchFragments()) {
			const size_t count = std::min(store.maximumBatchFragments(), locations.size() - first);
			store.releaseBatch(std::span<const NativeBulkRunFragmentLocation>(locations).subspan(first, count),
								checkpointLsn);
			if (first + count != locations.size()) {
				checkpointLsn = LogSequenceNumber(checkpointLsn.value() + 2U);
			}
		}
	}

	std::vector<std::byte> NativeBulkEdgeRunManifestCodec::encode(const NativeBulkEdgeRunManifest &manifest) {
		validateManifestShape(manifest, true);
		const std::vector<NativeBulkRunFragmentLocation> &items = references(manifest);
		const size_t referenceBytes = checkedMultiply(items.size(), RSS_NATIVE_BULK_EDGE_MANIFEST_REFERENCE_BYTES,
				"native bulk Edge manifest reference bytes overflowed");
		const size_t totalBytes = checkedAdd(RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES, referenceBytes,
				"native bulk Edge manifest size overflowed");
		if (totalBytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest exceeds one scratch segment");
		}
		std::vector<std::byte> result(totalBytes);
		LittleEndianCodec::writeU32(result, MAGIC_OFFSET, RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC);
		LittleEndianCodec::writeU16(result, VERSION_OFFSET, RSS_NATIVE_BULK_EDGE_MANIFEST_VERSION);
		LittleEndianCodec::writeU16(result, HEADER_BYTES_OFFSET, RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES);
		LittleEndianCodec::writeU16(result, FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU16(result, LEVEL_OFFSET, manifest.level);
		LittleEndianCodec::writeU32(result, REFERENCE_COUNT_OFFSET, static_cast<uint32_t>(items.size()));
		LittleEndianCodec::writeU64(result, RECORD_COUNT_OFFSET, manifest.record_count);
		LittleEndianCodec::writeU64(result, ENCODED_BYTES_OFFSET, manifest.encoded_bytes);
		LittleEndianCodec::writeU64(result, FIRST_EDGE_ID_OFFSET, manifest.first_edge_id);
		LittleEndianCodec::writeU64(result, LAST_EDGE_ID_OFFSET, manifest.last_edge_id);
		LittleEndianCodec::writeU64(result, TRANSACTION_ID_OFFSET, items.front().transaction_id);
		LittleEndianCodec::writeU32(result, REFERENCES_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(result, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED0_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED1_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED2_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED3_OFFSET, 0U);
		for (size_t ordinal = 0U; ordinal < items.size(); ++ordinal) {
			const size_t offset = RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES +
							  ordinal * RSS_NATIVE_BULK_EDGE_MANIFEST_REFERENCE_BYTES;
			LittleEndianCodec::writeU64(result, offset, items[ordinal].segment_id.value());
			LittleEndianCodec::writeU64(result, offset + sizeof(uint64_t), items[ordinal].generation.value());
		}
		const std::span<const std::byte> refs = std::span<const std::byte>(result).subspan(RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES);
		LittleEndianCodec::writeU32(result, REFERENCES_CRC_OFFSET, checksum(refs));
		LittleEndianCodec::writeU32(result, HEADER_CRC_OFFSET,
				checksum(std::span<const std::byte>(result).first(RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES)));
		return result;
	}

	NativeBulkEdgeRunManifest NativeBulkEdgeRunManifestCodec::decode(
			std::span<const std::byte> bytes, NativeBulkRunFragmentLocation location) {
		if (bytes.size() < RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES || !location.isValid() ||
			LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC ||
			LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_NATIVE_BULK_EDGE_MANIFEST_VERSION ||
			LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES ||
			LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, TRANSACTION_ID_OFFSET) != location.transaction_id ||
			LittleEndianCodec::readU64(bytes, RESERVED0_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, RESERVED1_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, RESERVED2_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, RESERVED3_OFFSET) != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest has an invalid header");
		}
		std::vector<std::byte> header(bytes.begin(), bytes.begin() + RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES);
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (checksum(header) != expectedHeaderCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge manifest header checksum does not match");
		}
		const uint32_t count = LittleEndianCodec::readU32(bytes, REFERENCE_COUNT_OFFSET);
		const size_t referenceBytes = checkedMultiply(count, RSS_NATIVE_BULK_EDGE_MANIFEST_REFERENCE_BYTES,
				"native bulk Edge manifest reference dimension overflowed");
		if (count == 0U || bytes.size() != RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES + referenceBytes) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest dimensions are invalid");
		}
		const std::span<const std::byte> refs = bytes.subspan(RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES);
		if (checksum(refs) != LittleEndianCodec::readU32(bytes, REFERENCES_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge manifest reference checksum does not match");
		}
		NativeBulkEdgeRunManifest result;
		result.location = location;
		result.level = LittleEndianCodec::readU16(bytes, LEVEL_OFFSET);
		result.record_count = LittleEndianCodec::readU64(bytes, RECORD_COUNT_OFFSET);
		result.encoded_bytes = LittleEndianCodec::readU64(bytes, ENCODED_BYTES_OFFSET);
		result.first_edge_id = LittleEndianCodec::readU64(bytes, FIRST_EDGE_ID_OFFSET);
		result.last_edge_id = LittleEndianCodec::readU64(bytes, LAST_EDGE_ID_OFFSET);
		std::vector<NativeBulkRunFragmentLocation> &items = result.level == 0U ? result.runs : result.children;
		items.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			const size_t offset = ordinal * RSS_NATIVE_BULK_EDGE_MANIFEST_REFERENCE_BYTES;
			NativeBulkRunFragmentLocation item{SegmentId(LittleEndianCodec::readU64(refs, offset)),
				Generation(LittleEndianCodec::readU64(refs, offset + sizeof(uint64_t))), location.transaction_id};
			if (!item.isValid() || item.segment_id == location.segment_id) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest has an invalid self or child reference");
			}
			items.push_back(item);
		}
		validateManifestShape(result, false);
		return result;
	}

	NativeBulkEdgeRunStreamingWriter::NativeBulkEdgeRunStreamingWriter(
			NativeBulkRunStore &store, uint64_t transactionId, NativeBulkEdgeRunStreamingOptions options,
			LogSequenceNumber firstCheckpointLsn) :
			store_(store), transaction_id_(transactionId), options_(options), next_checkpoint_lsn_(firstCheckpointLsn) {
		if (transaction_id_ == 0U || options_.maximum_records == 0U || options_.maximum_manifest_depth == 0U ||
			!next_checkpoint_lsn_.isValid() || options_.maximum_manifest_depth > store_.maximumManifestDepth() ||
			options_.maximum_run_bytes < RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES ||
			options_.maximum_run_bytes > store_.maximumFragmentBytes() || maximumManifestReferences() < 2U) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge streaming writer requires finite run, record, and manifest bounds");
		}
	}

	size_t NativeBulkEdgeRunStreamingWriter::maximumManifestReferences() const {
		return (store_.maximumFragmentBytes() - RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES) /
			   RSS_NATIVE_BULK_EDGE_MANIFEST_REFERENCE_BYTES;
	}

	NativeBulkEdgeRunManifest NativeBulkEdgeRunStreamingWriter::readManifest(
			NativeBulkRunFragmentLocation location) const {
		MetadataSnapshotPin snapshot = store_.metadata_.openSnapshot();
		NativeBulkEdgeRunManifest result =
				NativeBulkEdgeRunManifestCodec::decode(store_.readOpaquePayload(snapshot, location), location);
		snapshot.release();
		if (result.level >= options_.maximum_manifest_depth) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge manifest exceeds its configured depth");
		}
		return result;
	}

	NativeBulkEdgeRunManifest NativeBulkEdgeRunStreamingWriter::writeManifest(
			std::span<const NativeBulkRunFragmentLocation> items, uint16_t level) {
		if (items.empty() || level >= options_.maximum_manifest_depth) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge manifest requires bounded owned references");
		}
		MetadataSnapshotPin snapshot = store_.metadata_.openSnapshot();
		NativeBulkEdgeRunManifest result;
		result.level = level;
		std::optional<uint64_t> previousLast;
		for (const NativeBulkRunFragmentLocation &item: items) {
			if (item.transaction_id != transaction_id_) {
				snapshot.release();
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge manifest cannot reference a foreign run");
			}
			uint64_t first = 0U;
			uint64_t last = 0U;
			uint64_t count = 0U;
			uint64_t bytes = 0U;
			if (level == 0U) {
				const NativeBulkEdgeRun run = NativeBulkEdgeRunCodec::decode(store_.readOpaquePayload(snapshot, item));
				first = run.records.front().edge_id;
				last = run.records.back().edge_id;
				count = run.records.size();
				bytes = NativeBulkEdgeRunCodec::encodedSize(run);
				result.runs.push_back(item);
			} else {
				const NativeBulkEdgeRunManifest child =
						NativeBulkEdgeRunManifestCodec::decode(store_.readOpaquePayload(snapshot, item), item);
				if (child.level + 1U != level || child.level >= options_.maximum_manifest_depth) {
					snapshot.release();
					fail(FormatError::FERR_INVALID_STATE, "native bulk Edge manifest has a mismatched child level");
				}
				first = child.first_edge_id;
				last = child.last_edge_id;
				count = child.record_count;
				bytes = child.encoded_bytes;
				result.children.push_back(item);
			}
			if ((previousLast.has_value() && first <= *previousLast) || count == 0U || bytes == 0U ||
				result.record_count > std::numeric_limits<uint64_t>::max() - count ||
				result.encoded_bytes > std::numeric_limits<uint64_t>::max() - bytes) {
				snapshot.release();
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge manifest references are not globally ordered");
			}
			if (result.first_edge_id == 0U) {
				result.first_edge_id = first;
			}
			result.last_edge_id = last;
			result.record_count += count;
			result.encoded_bytes += bytes;
			previousLast = last;
		}
		snapshot.release();
		result.location = store_.writeOpaquePayload(transaction_id_, NativeBulkEdgeRunManifestCodec::encode(result),
			next_checkpoint_lsn_);
		return result;
	}

	void NativeBulkEdgeRunStreamingWriter::advanceCheckpointLsn() {
		// Each scratch write publishes both logical allocation metadata and the
		// sealed physical envelope. Keep subsequent staging writes beyond both
		// checkpoints so LSN ownership remains monotonic.
		if (!next_checkpoint_lsn_.isValid() ||
			next_checkpoint_lsn_.value() > std::numeric_limits<uint64_t>::max() - 2U) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge streaming writer checkpoint LSN overflowed");
		}
		next_checkpoint_lsn_ = LogSequenceNumber(next_checkpoint_lsn_.value() + 2U);
	}

	void NativeBulkEdgeRunStreamingWriter::flushRun() {
		if (buffered_.records.empty()) {
			return;
		}
		pending_run_payloads_.push_back(NativeBulkEdgeRunCodec::encode(buffered_));
		buffered_ = {};
		buffered_bytes_ = RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES;
		if (pending_run_payloads_.size() == store_.maximumBatchFragments()) {
			flushRunBatch();
		}
	}

	void NativeBulkEdgeRunStreamingWriter::flushRunBatch() {
		if (pending_run_payloads_.empty()) {
			return;
		}
		// Preserve the bounded in-memory batch until the metadata publication
		// succeeds. A stale-handle rebase can reject the first attempt; rollback
		// must still be able to seal and retire the original scratch run.
		const std::vector<NativeBulkRunFragmentLocation> locations = store_.writeOpaquePayloadBatch(
				transaction_id_, pending_run_payloads_, next_checkpoint_lsn_);
		pending_run_payloads_.clear();
		advanceCheckpointLsn();
		for (const NativeBulkRunFragmentLocation &location: locations) {
			appendManifestAtLevel(location, 0U);
		}
	}

	void NativeBulkEdgeRunStreamingWriter::appendManifestAtLevel(NativeBulkRunFragmentLocation location, size_t level) {
		if (level >= options_.maximum_manifest_depth) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge streaming writer exceeded its manifest depth");
		}
		if (manifest_levels_.size() <= level) {
			manifest_levels_.resize(level + 1U);
		}
		manifest_levels_[level].push_back(location);
		if (manifest_levels_[level].size() == maximumManifestReferences()) {
			sealManifestLevel(level);
		}
	}

	void NativeBulkEdgeRunStreamingWriter::sealManifestLevel(size_t level) {
		const uint16_t manifestLevel = static_cast<uint16_t>(level);
		const NativeBulkEdgeRunManifest manifest = writeManifest(manifest_levels_[level], manifestLevel);
		advanceCheckpointLsn();
		manifest_levels_[level].clear();
		appendManifestAtLevel(manifest.location, level + 1U);
	}

	void NativeBulkEdgeRunStreamingWriter::add(NativeBulkEdgeRecord record) {
		if (finished_ || record_count_ == options_.maximum_records || record.edge_id == 0U ||
			(last_edge_id_ != 0U && record.edge_id <= last_edge_id_)) {
			fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge streaming writer requires one bounded increasing record stream");
		}
		const size_t recordBytes = NativeBulkEdgeRecordCodec::encodedSize(record) + sizeof(uint32_t) + sizeof(uint32_t);
		if (recordBytes > options_.maximum_run_bytes - RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge record cannot fit in the configured run buffer");
		}
		if (buffered_bytes_ > options_.maximum_run_bytes - recordBytes) {
			flushRun();
		}
		last_edge_id_ = record.edge_id;
		buffered_bytes_ += recordBytes;
		buffered_.records.push_back(std::move(record));
		++record_count_;
	}

	void NativeBulkEdgeRunStreamingWriter::refreshCheckpointFloor(LogSequenceNumber checkpointLsn) {
		if (!checkpointLsn.isValid() || finished_) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge streaming writer requires an active representable checkpoint floor");
		}
		if (next_checkpoint_lsn_ < checkpointLsn) {
			next_checkpoint_lsn_ = checkpointLsn;
		}
	}

	NativeBulkEdgeRunManifest NativeBulkEdgeRunStreamingWriter::finish() {
		if (finished_ || record_count_ == 0U) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge streaming writer can finish one nonempty stream only once");
		}
		flushRun();
		flushRunBatch();
		// Level zero temporarily holds Edge-run locations rather than manifests.
		// Seal even one run so the returned root always has one typed manifest
		// codec and readers never need an ambiguous run-or-manifest discriminator.
		if (!manifest_levels_.empty() && !manifest_levels_.front().empty()) {
			sealManifestLevel(0U);
		}
		for (;;) {
			size_t roots = 0U;
			size_t highest = 0U;
			for (size_t level = 0U; level < manifest_levels_.size(); ++level) {
				if (!manifest_levels_[level].empty()) {
					roots += manifest_levels_[level].size();
					highest = level;
				}
			}
			if (roots == 1U) {
				NativeBulkEdgeRunManifest result = readManifest(manifest_levels_[highest].front());
				finished_ = true;
				return result;
			}
			bool progressed = false;
			for (size_t level = 0U; level <= highest; ++level) {
				if (manifest_levels_[level].empty()) {
					continue;
				}
				if (manifest_levels_[level].size() > 1U) {
					sealManifestLevel(level);
					progressed = true;
					break;
				}
				if (level < highest) {
					// A lower-level root cannot be attached directly to a higher-level
					// manifest: its child level would no longer match the parent. Seal it
					// first to create the required one-level wrapper.
					sealManifestLevel(level);
					progressed = true;
					break;
				}
			}
			if (!progressed) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge streaming writer could not seal one root");
			}
		}
	}

	void NativeBulkEdgeRunStreamingWriter::abort(LogSequenceNumber firstReleaseCheckpointLsn) {
		if (finished_ || record_count_ == 0U || !firstReleaseCheckpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge streaming writer can abort one nonempty active stream only once");
		}
		const NativeBulkEdgeRunManifest root = finish();
		// finish() can publish one or more manifest segments after the caller
		// obtained its release LSN. Retiring from that stale LSN would violate the
		// metadata batch monotonicity contract. Use the larger explicit caller LSN
		// or the post-finish metadata checkpoint plus one.
		MetadataSnapshotPin snapshot = store_.metadata_.openSnapshot();
		const uint64_t checkpoint = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		if (checkpoint == std::numeric_limits<uint64_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge streaming writer cannot advance the release checkpoint");
		}
		const LogSequenceNumber minimumReleaseCheckpoint(checkpoint + 1U);
		releaseNativeBulkEdgeManifest(
				store_, root.location,
				firstReleaseCheckpointLsn < minimumReleaseCheckpoint ? minimumReleaseCheckpoint : firstReleaseCheckpointLsn);
	}

} // namespace graph::storage
