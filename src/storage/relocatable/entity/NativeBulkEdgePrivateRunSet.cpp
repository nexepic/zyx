/**
 * @file NativeBulkEdgePrivateRunSet.cpp
 * @brief Bounded private logical-run collection implementation.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunSet.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = MAGIC_OFFSET + sizeof(uint32_t);
		constexpr size_t HEADER_BYTES_OFFSET = VERSION_OFFSET + sizeof(uint16_t);
		constexpr size_t FLAGS_OFFSET = HEADER_BYTES_OFFSET + sizeof(uint16_t);
		constexpr size_t LEVEL_OFFSET = FLAGS_OFFSET + sizeof(uint16_t);
		constexpr size_t REFERENCE_COUNT_OFFSET = LEVEL_OFFSET + sizeof(uint16_t);
		constexpr size_t RUN_COUNT_OFFSET = REFERENCE_COUNT_OFFSET + sizeof(uint32_t);
		constexpr size_t OWNER_OFFSET = RUN_COUNT_OFFSET + sizeof(uint64_t);
		constexpr size_t RECORD_CRC_OFFSET = OWNER_OFFSET + sizeof(uint64_t);
		// Keep the fixed header naturally aligned. Both reserved fields are
		// validated so a future format revision cannot be misread as v1.
		constexpr size_t RESERVED0_OFFSET = RECORD_CRC_OFFSET + sizeof(uint32_t);
		constexpr size_t RESERVED1_OFFSET = RESERVED0_OFFSET + sizeof(uint32_t);
		constexpr size_t REFERENCE_SEGMENT_OFFSET = 0U;
		constexpr size_t REFERENCE_GENERATION_OFFSET = sizeof(uint64_t);

		static_assert(RESERVED1_OFFSET + sizeof(uint64_t) == RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES);

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t crc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] size_t fanout() {
			constexpr size_t payloadBytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
			if (payloadBytes <= RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge private run-set cannot fit one candidate segment");
			}
			const size_t result =
					(payloadBytes - RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES) / RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES;
			if (result < 2U) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge private run-set fan-out is unusable");
			}
			return result;
		}

		[[nodiscard]] NativeBulkRunFragmentLocation
		locationFor(const NativeGraphPrivateSegmentMaterialization &identity) {
			if (!identity.isValid()) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge private run-set received an invalid identity");
			}
			return {identity.segment_id, identity.generation, identity.owner_id};
		}

		[[nodiscard]] NativeGraphPrivateSegmentMaterialization
		identityFor(const NativeGraphPrivateSegmentSet &identities, NativeBulkRunFragmentLocation location) {
			return identities[identities.ordinalOf(
					{location.segment_id, location.generation, location.transaction_id})];
		}

		class PrivateRunSetCursor final {
		public:
			PrivateRunSetCursor(const NativeGraphPrivateSegmentSet &identities, NativeBulkRunFragmentLocation root,
								uint16_t maximumDepth, NativeGraphPrivateSegmentReader reader) :
				identities_(identities), maximum_depth_(maximumDepth), reader_(std::move(reader)) {
				if (maximum_depth_ == 0U || !reader_) {
					fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run-set cursor is invalid");
				}
				stack_.push_back({read(root), 0U});
			}

			[[nodiscard]] std::optional<OrderedBTreeEntrySource> next() {
				while (!stack_.empty()) {
					Frame &frame = stack_.back();
					if (frame.manifest.level == 0U) {
						if (frame.ordinal == frame.manifest.runs.size()) {
							completeTopFrame();
							continue;
						}
						const NativeBulkRunFragmentLocation run = frame.manifest.runs[frame.ordinal++];
						if (frame.emitted_runs == frame.manifest.run_count) {
							fail(FormatError::FERR_INVALID_HEADER,
								 "native bulk Edge private run-set contains more runs than declared");
						}
						++frame.emitted_runs;
						return NativeBulkRunStore::orderedSourceFromPayloadReader(run, maximum_depth_, payloadReader());
					}
					if (frame.ordinal == frame.manifest.children.size()) {
						completeTopFrame();
						continue;
					}
					if (stack_.size() >= maximum_depth_) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge private run-set exceeds its bounded hierarchy depth");
					}
					const NativeBulkEdgePrivateRunSetManifest child = read(frame.manifest.children[frame.ordinal++]);
					if (child.level + 1U != frame.manifest.level) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge private run-set has an invalid child level");
					}
					stack_.push_back({child, 0U});
				}
				return std::nullopt;
			}

		private:
			struct Frame {
				NativeBulkEdgePrivateRunSetManifest manifest;
				size_t ordinal = 0U;
				uint64_t emitted_runs = 0U;
			};

			void completeTopFrame() {
				if (stack_.back().emitted_runs != stack_.back().manifest.run_count) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge private run-set run count does not match its traversal");
				}
				const uint64_t completedRuns = stack_.back().emitted_runs;
				stack_.pop_back();
				if (!stack_.empty()) {
					Frame &parent = stack_.back();
					if (completedRuns > parent.manifest.run_count - parent.emitted_runs) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "native bulk Edge private run-set child counts exceed the parent count");
					}
					parent.emitted_runs += completedRuns;
				}
			}

			[[nodiscard]] NativeBulkEdgePrivateRunSetManifest read(NativeBulkRunFragmentLocation location) const {
				return NativeBulkEdgePrivateRunSetCodec::decode(reader_(identityFor(identities_, location)), location);
			}

			[[nodiscard]] NativeBulkRunPayloadReader payloadReader() const {
				return [identities = &identities_, reader = reader_](const NativeBulkRunFragmentLocation &location) {
					return reader(identityFor(*identities, location));
				};
			}

			const NativeGraphPrivateSegmentSet &identities_;
			uint16_t maximum_depth_ = 0U;
			NativeGraphPrivateSegmentReader reader_;
			std::vector<Frame> stack_;
		};

	} // namespace

	std::vector<std::byte>
	NativeBulkEdgePrivateRunSetCodec::encode(const NativeBulkEdgePrivateRunSetManifest &manifest) {
		const std::vector<NativeBulkRunFragmentLocation> &references =
				manifest.level == 0U ? manifest.runs : manifest.children;
		if (manifest.location.isValid() || references.empty() || manifest.run_count == 0U ||
			(manifest.level == 0U && !manifest.children.empty()) || (manifest.level != 0U && !manifest.runs.empty()) ||
			references.size() > fanout()) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run-set manifest shape is invalid");
		}
		const uint64_t owner = references.front().transaction_id;
		if (owner == 0U || std::ranges::any_of(references, [owner](const NativeBulkRunFragmentLocation &location) {
				return !location.isValid() || location.transaction_id != owner;
			})) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge private run-set references have invalid ownership");
		}
		const size_t bytes =
				RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES + references.size() * RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES;
		std::vector<std::byte> result(bytes);
		LittleEndianCodec::writeU32(result, MAGIC_OFFSET, RSS_NATIVE_BULK_RUN_SET_MAGIC);
		LittleEndianCodec::writeU16(result, VERSION_OFFSET, RSS_NATIVE_BULK_RUN_SET_VERSION);
		LittleEndianCodec::writeU16(result, HEADER_BYTES_OFFSET, RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES);
		LittleEndianCodec::writeU16(result, FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU16(result, LEVEL_OFFSET, manifest.level);
		LittleEndianCodec::writeU32(result, REFERENCE_COUNT_OFFSET, static_cast<uint32_t>(references.size()));
		LittleEndianCodec::writeU64(result, RUN_COUNT_OFFSET, manifest.run_count);
		LittleEndianCodec::writeU64(result, OWNER_OFFSET, owner);
		LittleEndianCodec::writeU32(result, RESERVED0_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED1_OFFSET, 0U);
		for (size_t ordinal = 0U; ordinal < references.size(); ++ordinal) {
			const size_t offset =
					RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES + ordinal * RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES;
			LittleEndianCodec::writeU64(result, offset + REFERENCE_SEGMENT_OFFSET,
										references[ordinal].segment_id.value());
			LittleEndianCodec::writeU64(result, offset + REFERENCE_GENERATION_OFFSET,
										references[ordinal].generation.value());
		}
		LittleEndianCodec::writeU32(
				result, RECORD_CRC_OFFSET,
				crc(std::span<const std::byte>(result).subspan(RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES)));
		return result;
	}

	NativeBulkEdgePrivateRunSetManifest
	NativeBulkEdgePrivateRunSetCodec::decode(std::span<const std::byte> payload,
											 NativeBulkRunFragmentLocation location) {
		if (!location.isValid() || payload.size() < RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "native bulk Edge private run-set header is truncated");
		}
		const uint32_t references = LittleEndianCodec::readU32(payload, REFERENCE_COUNT_OFFSET);
		const uint8_t invalid =
				static_cast<uint8_t>(LittleEndianCodec::readU32(payload, MAGIC_OFFSET) !=
									 RSS_NATIVE_BULK_RUN_SET_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(payload, VERSION_OFFSET) !=
									 RSS_NATIVE_BULK_RUN_SET_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(payload, HEADER_BYTES_OFFSET) !=
									 RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(payload, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(payload, OWNER_OFFSET) != location.transaction_id) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(payload, RESERVED0_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(payload, RESERVED1_OFFSET) != 0U) |
				static_cast<uint8_t>(references == 0U || references > fanout()) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(payload, RUN_COUNT_OFFSET) == 0U) |
				static_cast<uint8_t>(payload.size() !=
									 RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES +
											 static_cast<size_t>(references) * RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run-set manifest is invalid");
		}
		if (LittleEndianCodec::readU32(payload, RECORD_CRC_OFFSET) !=
			crc(payload.subspan(RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES))) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH,
				 "native bulk Edge private run-set manifest checksum does not match");
		}
		NativeBulkEdgePrivateRunSetManifest result;
		result.location = location;
		result.level = LittleEndianCodec::readU16(payload, LEVEL_OFFSET);
		result.run_count = LittleEndianCodec::readU64(payload, RUN_COUNT_OFFSET);
		auto &target = result.level == 0U ? result.runs : result.children;
		target.reserve(references);
		for (size_t ordinal = 0U; ordinal < references; ++ordinal) {
			const size_t offset =
					RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES + ordinal * RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES;
			NativeBulkRunFragmentLocation reference{
					SegmentId(LittleEndianCodec::readU64(payload, offset)),
					Generation(LittleEndianCodec::readU64(payload, offset + REFERENCE_GENERATION_OFFSET)),
					location.transaction_id};
			if (!reference.isValid() || reference.segment_id == location.segment_id) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run-set reference is invalid");
			}
			target.push_back(reference);
		}
		return result;
	}

	NativeBulkEdgePrivateRunSetBuilder::NativeBulkEdgePrivateRunSetBuilder(
			uint16_t maximumDepth, const NativeBulkEdgePrivateCandidateAllocator &allocateCandidate,
			const NativeBulkEdgePrivateCandidateWriter &writeCandidate) :
		maximum_depth_(maximumDepth), allocate_candidate_(allocateCandidate), write_candidate_(writeCandidate) {
		if (maximum_depth_ == 0U || !allocate_candidate_ || !write_candidate_) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run-set builder is invalid");
		}
		levels_.resize(static_cast<size_t>(maximum_depth_) + 1U);
		for (auto &level: levels_) {
			level.reserve(fanout());
		}
	}

	void NativeBulkEdgePrivateRunSetBuilder::append(NativeBulkRunFragmentLocation runRoot) {
		appendAtLevel(0U, runRoot, 1U);
	}

	void NativeBulkEdgePrivateRunSetBuilder::appendAtLevel(uint16_t level, NativeBulkRunFragmentLocation location,
														   uint64_t runCount) {
		if (level > maximum_depth_ || !location.isValid() || runCount == 0U) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge private run-set builder received an invalid run");
		}
		auto &items = levels_[level];
		items.emplace_back(location, runCount);
		if (level == maximum_depth_) {
			if (items.size() != 1U) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge private run-set exceeded its depth bound");
			}
			return;
		}
		if (items.size() == fanout()) {
			flushLevel(level);
		}
	}

	void NativeBulkEdgePrivateRunSetBuilder::flushLevel(uint16_t level) {
		auto items = std::move(levels_[level]);
		levels_[level].clear();
		if (items.empty()) {
			return;
		}
		NativeBulkEdgePrivateRunSetManifest manifest;
		manifest.level = level;
		auto &references = level == 0U ? manifest.runs : manifest.children;
		references.reserve(items.size());
		for (const auto &[location, count]: items) {
			if (location.transaction_id != items.front().first.transaction_id ||
				count > std::numeric_limits<uint64_t>::max() - manifest.run_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge private run-set has invalid run ownership or count");
			}
			references.push_back(location);
			manifest.run_count += count;
		}
		const std::vector<std::byte> payload = NativeBulkEdgePrivateRunSetCodec::encode(manifest);
		const NativeGraphPrivateSegmentMaterialization identity = allocate_candidate_();
		if (identity.owner_id != items.front().first.transaction_id) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge private run-set crosses candidate-owner boundaries");
		}
		write_candidate_(identity, payload);
		appendAtLevel(static_cast<uint16_t>(level + 1U), locationFor(identity), manifest.run_count);
	}

	NativeBulkRunFragmentLocation NativeBulkEdgePrivateRunSetBuilder::finish() {
		const bool hasAnyLevel = std::ranges::any_of(
				levels_.begin(), levels_.end(), [](const auto &items) { return !items.empty(); });
		if (!hasAnyLevel) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge private run-set cannot finish without a run");
		}
		for (;;) {
			size_t lowest = 0U;
			while (lowest < levels_.size() && levels_[lowest].empty()) {
				++lowest;
			}
			if (lowest == levels_.size()) {
				fail(FormatError::FERR_INVALID_STATE, "native bulk Edge private run-set lost its root");
			}
			const bool hasHigher = std::ranges::any_of(levels_.begin() + static_cast<std::ptrdiff_t>(lowest + 1U),
													   levels_.end(), [](const auto &items) { return !items.empty(); });
			if (lowest != 0U && !hasHigher && levels_[lowest].size() == 1U) {
				return levels_[lowest].front().first;
			}
			flushLevel(static_cast<uint16_t>(lowest));
		}
	}

	NativeBulkEdgeRunSource nativeBulkEdgePrivateRunSetSource(const NativeGraphPrivateSegmentSet &identities,
														  NativeBulkRunFragmentLocation root, uint16_t maximumDepth,
														  const NativeGraphPrivateSegmentReader &reader) {
		if (!root.isValid() || maximumDepth == 0U || !reader) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run-set source is invalid");
		}
		static_cast<void>(identityFor(identities, root));
		return [&identities, root, maximumDepth, reader] {
			auto cursor = std::make_shared<PrivateRunSetCursor>(identities, root, maximumDepth, reader);
			return [cursor]() { return cursor->next(); };
		};
	}

	NativeBulkRunFragmentLocation
	nativeBulkEdgePrivateRunSetSingleRun(const NativeGraphPrivateSegmentSet &identities,
									  NativeBulkRunFragmentLocation root,
									  const NativeGraphPrivateSegmentReader &reader) {
		if (!root.isValid() || !reader) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run-set terminal root is invalid");
		}
		const NativeBulkEdgePrivateRunSetManifest manifest =
				NativeBulkEdgePrivateRunSetCodec::decode(reader(identityFor(identities, root)), root);
		if (manifest.level != 0U || manifest.run_count != 1U || manifest.runs.size() != 1U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge private run-set does not contain exactly one terminal run");
		}
		return manifest.runs.front();
	}

} // namespace graph::storage
