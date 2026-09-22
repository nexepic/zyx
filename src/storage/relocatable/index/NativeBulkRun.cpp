/**
 * @file NativeBulkRun.cpp
 * @brief Checked transaction-private bulk-run fragment codec implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/NativeBulkRun.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t FLAGS_OFFSET = 8U;
		constexpr size_t RECORD_COUNT_OFFSET = 12U;
		constexpr size_t RECORD_BYTES_OFFSET = 16U;
		constexpr size_t RECORD_CRC_OFFSET = 20U;
		constexpr size_t LOGICAL_BYTES_OFFSET = 24U;
		constexpr size_t RESERVED0_OFFSET = 32U;
		constexpr size_t RESERVED1_OFFSET = 40U;
		constexpr size_t RESERVED2_OFFSET = 48U;
		constexpr size_t RESERVED3_OFFSET = 56U;
		constexpr size_t RECORD_LENGTH_OFFSET = 0U;
		constexpr size_t RECORD_CRC_OFFSET_IN_RECORD = 4U;
		constexpr size_t MAXIMUM_ENTRY_BYTES = RSS_BTREE_ENTRY_HEADER_BYTES + RSS_INDEX_KEY_MAX_BYTES;
		constexpr size_t MANIFEST_MAGIC_OFFSET = 0U;
		constexpr size_t MANIFEST_VERSION_OFFSET = 4U;
		constexpr size_t MANIFEST_HEADER_BYTES_OFFSET = 6U;
		constexpr size_t MANIFEST_FLAGS_OFFSET = 8U;
		constexpr size_t MANIFEST_LEVEL_OFFSET = 10U;
		constexpr size_t MANIFEST_FRAGMENT_COUNT_OFFSET = 12U;
		constexpr size_t MANIFEST_ENTRY_COUNT_OFFSET = 16U;
		constexpr size_t MANIFEST_ENCODED_BYTES_OFFSET = 24U;
		constexpr size_t MANIFEST_RECORD_CRC_OFFSET = 32U;
		constexpr size_t MANIFEST_RESERVED_PADDING_OFFSET = 36U;
		constexpr size_t MANIFEST_TRANSACTION_ID_OFFSET = 40U;
		constexpr size_t MANIFEST_RESERVED0_OFFSET = 48U;
		constexpr size_t MANIFEST_RESERVED1_OFFSET = 56U;
		constexpr size_t MANIFEST_FRAGMENT_SEGMENT_ID_OFFSET = 0U;
		constexpr size_t MANIFEST_FRAGMENT_GENERATION_OFFSET = 8U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t crc(std::span<const std::byte> bytes) {
			const std::byte empty{};
			return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
		}

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(LogSequenceNumber checkpointLsn) {
			if (!checkpointLsn.isValid() || checkpointLsn.value() == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable native bulk-run checkpoint LSN cannot advance for scratch allocation");
			}
			return LogSequenceNumber(checkpointLsn.value() + 1U);
		}

		[[nodiscard]] LogSequenceNumber advanceCheckpointLsn(LogSequenceNumber checkpointLsn, uint64_t steps) {
			if (!checkpointLsn.isValid() || steps > std::numeric_limits<uint64_t>::max() - checkpointLsn.value()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable native bulk-run checkpoint LSN cannot advance during recovery");
			}
			return LogSequenceNumber(checkpointLsn.value() + steps);
		}


		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		void validateEntry(const BTreePageEntry &entry) {
			if (BTreePageEntryCodec::encodedSize(entry) > MAXIMUM_ENTRY_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable native bulk-run entry exceeds the bounded B+Tree entry framing");
			}
		}

		void validateManifestShape(const NativeBulkRunManifest &manifest) {
			const std::vector<NativeBulkRunFragmentLocation> &references =
					manifest.level == 0U ? manifest.fragments : manifest.children;
			if ((manifest.level == 0U && !manifest.children.empty()) ||
				(manifest.level != 0U && !manifest.fragments.empty()) || references.empty() ||
				references.size() > std::numeric_limits<uint32_t>::max() ||
				manifest.entry_count == 0U || manifest.encoded_bytes == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable native bulk-run manifest has invalid bounded dimensions");
			}
			const uint64_t transactionId = references.front().transaction_id;
			if (transactionId == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable native bulk-run manifest has no transaction owner");
			}
			for (const NativeBulkRunFragmentLocation &fragment: references) {
				if (!fragment.isValid() || fragment.transaction_id != transactionId) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable native bulk-run manifest has a foreign or invalid fragment location");
				}
			}
		}

		class NativeBulkRunManifestCursor final {
		public:
			NativeBulkRunManifestCursor(NativeBulkRunManifest manifest, uint16_t maximumDepth,
								 NativeBulkRunPayloadReader payloadReader) :
				manifest_(std::move(manifest)), payload_reader_(std::move(payloadReader)), maximum_depth_(maximumDepth) {}

			[[nodiscard]] std::optional<BTreePageEntry> next() {
				for (;;) {
					if (entry_ordinal_ < fragment_.entries.size()) {
						BTreePageEntry entry = std::move(fragment_.entries[entry_ordinal_++]);
						++emitted_entries_;
						return entry;
					}
					if (fragment_ordinal_ == leaf_fragments_.size()) {
						if (!advanceToNextLeaf()) {
							if (emitted_entries_ != manifest_.entry_count) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable native bulk-run manifest entry count does not match its fragments");
						}
							return std::nullopt;
						}
					}
					{
						debug::ScopedPerfTimer timer("native_bulk_run.fragment_read_decode");
						fragment_ = NativeBulkRunFragmentCodec::decode(payload_reader_(leaf_fragments_[fragment_ordinal_++]));
					}
					// Fragment decoding already authenticates its internal strict order. A
					// single boundary check therefore proves global order without copying
					// every key as this forward-only cursor consumes the fragment.
					if (last_key_.has_value() && !lessKey(*last_key_, fragment_.entries.front().key)) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk-run manifest fragments are not globally ordered");
					}
					last_key_ = fragment_.entries.back().key;
					entry_ordinal_ = 0U;
				}
			}

		private:
			struct ManifestFrame {
				NativeBulkRunManifest manifest;
				size_t child_ordinal = 0U;
			};

			[[nodiscard]] bool advanceToNextLeaf() {
				if (!started_) {
					started_ = true;
					stack_.push_back({manifest_, 0U});
				}
				while (!stack_.empty()) {
					ManifestFrame &frame = stack_.back();
					if (frame.manifest.level == 0U) {
						// Decoding rejects zero-reference manifests before this private cursor is created.
						leaf_fragments_ = std::move(frame.manifest.fragments);
						fragment_ordinal_ = 0U;
						stack_.pop_back();
						return true;
					}
					if (frame.child_ordinal == frame.manifest.children.size()) {
						stack_.pop_back();
						continue;
					}
					const NativeBulkRunFragmentLocation child = frame.manifest.children[frame.child_ordinal++];
					NativeBulkRunManifest childManifest =
							NativeBulkRunManifestCodec::decode(payload_reader_(child), child);
					// `maximum_depth` is the exclusive upper bound for the manifest
					// level: level zero is the leaf, so a configured depth of N
					// permits levels [0, N).  Keep the cursor consistent with the
					// writer's append/seal checks at the boundary.
					if (childManifest.level + 1U != frame.manifest.level ||
						stack_.size() >= static_cast<size_t>(maximum_depth_)) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk-run manifest hierarchy has an invalid depth or child level");
					}
					stack_.push_back({std::move(childManifest), 0U});
				}
				return false;
			}

			NativeBulkRunManifest manifest_;
			NativeBulkRunPayloadReader payload_reader_;
			NativeBulkRunFragment fragment_;
			std::vector<NativeBulkRunFragmentLocation> leaf_fragments_;
			std::vector<ManifestFrame> stack_;
			bool started_ = false;
			uint16_t maximum_depth_ = 0U;
			size_t fragment_ordinal_ = 0U;
			size_t entry_ordinal_ = 0U;
			uint64_t emitted_entries_ = 0U;
			std::optional<std::vector<std::byte>> last_key_;
		};

		struct NativeBulkRunMergeItem {
			size_t cursor_ordinal = 0U;
			BTreePageEntry entry;
		};

		struct NativeBulkRunMergeItemGreater {
			[[nodiscard]] bool operator()(const NativeBulkRunMergeItem &left, const NativeBulkRunMergeItem &right) const {
				if (lessKey(right.entry.key, left.entry.key)) {
					return true;
				}
				if (lessKey(left.entry.key, right.entry.key)) {
					return false;
				}
				return left.cursor_ordinal > right.cursor_ordinal;
			}
		};

		struct NativeBulkRunMergedSourceState {
			std::vector<OrderedBTreeEntryCursor> cursors;
			std::vector<std::optional<std::vector<std::byte>>> previous_input_keys;
			std::vector<NativeBulkRunMergeItem> ready;
			std::optional<std::vector<std::byte>> previous_output_key;
			size_t maximum_entries = 0U;
			size_t emitted_entries = 0U;

			void addNext(size_t cursorOrdinal) {
				std::optional<BTreePageEntry> next = cursors[cursorOrdinal]();
				if (!next.has_value()) {
					return;
				}
				if (next->key.empty() ||
					(previous_input_keys[cursorOrdinal].has_value() &&
						 !lessKey(*previous_input_keys[cursorOrdinal], next->key))) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable bounded bulk-run merge received an unordered input source");
				}
				previous_input_keys[cursorOrdinal] = next->key;
				ready.push_back({cursorOrdinal, std::move(*next)});
				std::push_heap(ready.begin(), ready.end(), NativeBulkRunMergeItemGreater{});
			}

			[[nodiscard]] std::optional<BTreePageEntry> next() {
				if (ready.empty()) {
					return std::nullopt;
				}
				std::pop_heap(ready.begin(), ready.end(), NativeBulkRunMergeItemGreater{});
				NativeBulkRunMergeItem item = std::move(ready.back());
				ready.pop_back();
				if (emitted_entries == maximum_entries ||
					(previous_output_key.has_value() && !lessKey(*previous_output_key, item.entry.key))) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable bounded bulk-run merge exceeded its entry bound or found a duplicate key");
				}
				previous_output_key = item.entry.key;
				++emitted_entries;
				addNext(item.cursor_ordinal);
				return std::move(item.entry);
			}
		};

		[[nodiscard]] std::vector<std::byte> encodeEntries(const NativeBulkRunFragment &fragment) {
			std::vector<std::byte> records;
			for (size_t index = 0U; index < fragment.entries.size(); ++index) {
				const BTreePageEntry &entry = fragment.entries[index];
				validateEntry(entry);
				if (index != 0U && !lessKey(fragment.entries[index - 1U].key, entry.key)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable native bulk-run entries must have strictly increasing keys");
				}
				const std::vector<std::byte> encoded = BTreePageEntryCodec::encode(entry);
				if (records.size() > std::numeric_limits<size_t>::max() -
								(RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + encoded.size())) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "relocatable native bulk-run record bytes overflow");
				}
				const size_t offset = records.size();
				records.resize(offset + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + encoded.size());
				LittleEndianCodec::writeU32(records, offset + RECORD_LENGTH_OFFSET, static_cast<uint32_t>(encoded.size()));
				LittleEndianCodec::writeU32(records, offset + RECORD_CRC_OFFSET_IN_RECORD, crc(encoded));
				std::copy(encoded.begin(), encoded.end(),
						  records.begin() + static_cast<std::ptrdiff_t>(offset + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES));
			}
			return records;
		}


	} // namespace

	NativeBulkRunFragmentLayoutBuilder::NativeBulkRunFragmentLayoutBuilder(const size_t maximumFragmentBytes) :
		maximum_fragment_bytes_(maximumFragmentBytes) {
		if (maximum_fragment_bytes_ <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			maximum_fragment_bytes_ > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run fragment layout requires a bounded segment payload");
		}
	}

	void NativeBulkRunFragmentLayoutBuilder::append(const size_t encodedEntryBytes) {
		if (finished_ || encodedEntryBytes == 0U ||
			encodedEntryBytes > maximum_fragment_bytes_ - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run fragment layout received an invalid encoded entry size");
		}
		if (current_encoded_bytes_ > maximum_fragment_bytes_ - encodedEntryBytes) {
			flush();
		}
		if (layout_.entry_count == std::numeric_limits<uint64_t>::max() ||
			current_entry_count_ == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "relocatable native bulk-run fragment layout entry count overflowed");
		}
		current_encoded_bytes_ += encodedEntryBytes;
		++current_entry_count_;
		++layout_.entry_count;
	}

	NativeBulkRunFragmentLayout NativeBulkRunFragmentLayoutBuilder::finish() {
		if (finished_ || layout_.entry_count == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run fragment layout requires one unfinished nonempty input");
		}
		flush();
		finished_ = true;
		return std::move(layout_);
	}

	void NativeBulkRunFragmentLayoutBuilder::flush() {
		if (current_entry_count_ == 0U) {
			return;
		}
		if (current_encoded_bytes_ > std::numeric_limits<uint64_t>::max() - layout_.encoded_bytes) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "relocatable native bulk-run fragment layout byte count overflowed");
		}
		layout_.fragments.push_back(
				{current_first_entry_ordinal_, current_entry_count_, current_encoded_bytes_});
		layout_.encoded_bytes += current_encoded_bytes_;
		if (current_entry_count_ > std::numeric_limits<size_t>::max() - current_first_entry_ordinal_) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "relocatable native bulk-run fragment layout source range overflowed");
		}
		current_first_entry_ordinal_ += current_entry_count_;
		current_entry_count_ = 0U;
		current_encoded_bytes_ = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
	}

	std::vector<std::byte> NativeBulkRunFragmentCodec::encode(const NativeBulkRunFragment &fragment) {
		const std::vector<std::byte> records = encodeEntries(fragment);
		const uint64_t totalBytes = static_cast<uint64_t>(RSS_NATIVE_BULK_RUN_HEADER_BYTES) + records.size();
		if (fragment.entries.empty() || fragment.entries.size() > std::numeric_limits<uint32_t>::max() ||
			records.size() > std::numeric_limits<uint32_t>::max() || totalBytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run fragment has invalid bounded dimensions");
		}

		std::vector<std::byte> bytes(static_cast<size_t>(totalBytes));
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_NATIVE_BULK_RUN_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_NATIVE_BULK_RUN_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_NATIVE_BULK_RUN_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU16(bytes, FLAGS_OFFSET + sizeof(uint16_t), 0U);
		LittleEndianCodec::writeU32(bytes, RECORD_COUNT_OFFSET, static_cast<uint32_t>(fragment.entries.size()));
		LittleEndianCodec::writeU32(bytes, RECORD_BYTES_OFFSET, static_cast<uint32_t>(records.size()));
		LittleEndianCodec::writeU32(bytes, RECORD_CRC_OFFSET, crc(records));
		LittleEndianCodec::writeU64(bytes, LOGICAL_BYTES_OFFSET, records.size());
		LittleEndianCodec::writeU64(bytes, RESERVED0_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, RESERVED1_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, RESERVED2_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, RESERVED3_OFFSET, 0U);
		std::copy(records.begin(), records.end(), bytes.begin() + RSS_NATIVE_BULK_RUN_HEADER_BYTES);
		return bytes;
	}

	NativeBulkRunFragment NativeBulkRunFragmentCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable native bulk-run header is truncated");
		}
		const uint8_t invalidHeader =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_NATIVE_BULK_RUN_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_NATIVE_BULK_RUN_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_NATIVE_BULK_RUN_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, FLAGS_OFFSET + sizeof(uint16_t)) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, RESERVED0_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, RESERVED1_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, RESERVED2_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, RESERVED3_OFFSET) != 0U);
		if (invalidHeader != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run header is invalid");
		}
		const uint32_t recordCount = LittleEndianCodec::readU32(bytes, RECORD_COUNT_OFFSET);
		const uint32_t recordBytes = LittleEndianCodec::readU32(bytes, RECORD_BYTES_OFFSET);
		const uint64_t logicalBytes = LittleEndianCodec::readU64(bytes, LOGICAL_BYTES_OFFSET);
		if (recordCount == 0U || logicalBytes != recordBytes || recordBytes != bytes.size() - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run dimensions are invalid");
		}
		const std::span<const std::byte> records = bytes.subspan(RSS_NATIVE_BULK_RUN_HEADER_BYTES);
		if (LittleEndianCodec::readU32(bytes, RECORD_CRC_OFFSET) != crc(records)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "relocatable native bulk-run records checksum does not match");
		}

		NativeBulkRunFragment result;
		result.entries.reserve(recordCount);
		size_t offset = 0U;
		while (offset < records.size()) {
			if (records.size() - offset < RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable native bulk-run record header is truncated");
			}
			const uint32_t entryBytes = LittleEndianCodec::readU32(records, offset + RECORD_LENGTH_OFFSET);
			if (entryBytes < RSS_BTREE_ENTRY_HEADER_BYTES || entryBytes > MAXIMUM_ENTRY_BYTES ||
				entryBytes > records.size() - offset - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run record length is invalid");
			}
			const std::span<const std::byte> encoded = records.subspan(offset + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES,
																						 entryBytes);
			if (LittleEndianCodec::readU32(records, offset + RECORD_CRC_OFFSET_IN_RECORD) != crc(encoded)) {
				fail(FormatError::FERR_CHECKSUM_MISMATCH, "relocatable native bulk-run record checksum does not match");
			}
			BTreePageEntry entry = BTreePageEntryCodec::decode(encoded);
			if (!result.entries.empty() && !lessKey(result.entries.back().key, entry.key)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable native bulk-run entries are not strictly ordered");
			}
			result.entries.push_back(std::move(entry));
			offset += RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + entryBytes;
		}
		if (result.entries.size() != recordCount) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run record count does not match");
		}
		return result;
	}

	std::vector<std::byte> NativeBulkRunManifestCodec::encode(const NativeBulkRunManifest &manifest) {
		validateManifestShape(manifest);
		if (manifest.location.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run manifest encoding must not contain its own location");
		}
		const std::vector<NativeBulkRunFragmentLocation> &references =
				manifest.level == 0U ? manifest.fragments : manifest.children;
		const uint64_t recordBytes = references.size() * RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES;
		if (recordBytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES - RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run manifest exceeds one bounded scratch segment");
		}
		std::vector<std::byte> bytes(RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES + static_cast<size_t>(recordBytes));
		LittleEndianCodec::writeU32(bytes, MANIFEST_MAGIC_OFFSET, RSS_NATIVE_BULK_RUN_MANIFEST_MAGIC);
		LittleEndianCodec::writeU16(bytes, MANIFEST_VERSION_OFFSET, RSS_NATIVE_BULK_RUN_MANIFEST_VERSION);
		LittleEndianCodec::writeU16(bytes, MANIFEST_HEADER_BYTES_OFFSET, RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, MANIFEST_FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU16(bytes, MANIFEST_LEVEL_OFFSET, manifest.level);
		LittleEndianCodec::writeU32(bytes, MANIFEST_FRAGMENT_COUNT_OFFSET, static_cast<uint32_t>(references.size()));
		LittleEndianCodec::writeU64(bytes, MANIFEST_ENTRY_COUNT_OFFSET, manifest.entry_count);
		LittleEndianCodec::writeU64(bytes, MANIFEST_ENCODED_BYTES_OFFSET, manifest.encoded_bytes);
		LittleEndianCodec::writeU64(bytes, MANIFEST_TRANSACTION_ID_OFFSET, references.front().transaction_id);
		LittleEndianCodec::writeU32(bytes, MANIFEST_RESERVED_PADDING_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, MANIFEST_RESERVED0_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, MANIFEST_RESERVED1_OFFSET, 0U);
		for (size_t ordinal = 0U; ordinal < references.size(); ++ordinal) {
			const size_t offset = RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES +
								  ordinal * RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES;
			LittleEndianCodec::writeU64(bytes, offset + MANIFEST_FRAGMENT_SEGMENT_ID_OFFSET,
										references[ordinal].segment_id.value());
			LittleEndianCodec::writeU64(bytes, offset + MANIFEST_FRAGMENT_GENERATION_OFFSET,
										references[ordinal].generation.value());
		}
		LittleEndianCodec::writeU32(bytes, MANIFEST_RECORD_CRC_OFFSET,
								 crc(std::span<const std::byte>(bytes).subspan(RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES)));
		return bytes;
	}

	NativeBulkRunManifest NativeBulkRunManifestCodec::decode(std::span<const std::byte> bytes,
														 NativeBulkRunFragmentLocation location) {
		if (!location.isValid() || bytes.size() < RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable native bulk-run manifest header is truncated or unowned");
		}
		const uint8_t invalidHeader =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MANIFEST_MAGIC_OFFSET) != RSS_NATIVE_BULK_RUN_MANIFEST_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, MANIFEST_VERSION_OFFSET) != RSS_NATIVE_BULK_RUN_MANIFEST_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, MANIFEST_HEADER_BYTES_OFFSET) != RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, MANIFEST_FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MANIFEST_RESERVED_PADDING_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, MANIFEST_TRANSACTION_ID_OFFSET) != location.transaction_id) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, MANIFEST_RESERVED0_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, MANIFEST_RESERVED1_OFFSET) != 0U);
		const uint32_t fragmentCount = LittleEndianCodec::readU32(bytes, MANIFEST_FRAGMENT_COUNT_OFFSET);
		if (invalidHeader != 0U || fragmentCount == 0U ||
			bytes.size() != RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES +
								static_cast<size_t>(fragmentCount) * RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES ||
			LittleEndianCodec::readU64(bytes, MANIFEST_ENTRY_COUNT_OFFSET) == 0U ||
			LittleEndianCodec::readU64(bytes, MANIFEST_ENCODED_BYTES_OFFSET) == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run manifest dimensions are invalid");
		}
		const std::span<const std::byte> records = bytes.subspan(RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES);
		if (LittleEndianCodec::readU32(bytes, MANIFEST_RECORD_CRC_OFFSET) != crc(records)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "relocatable native bulk-run manifest checksum does not match");
		}
		NativeBulkRunManifest result;
		result.location = location;
		result.level = LittleEndianCodec::readU16(bytes, MANIFEST_LEVEL_OFFSET);
		result.entry_count = LittleEndianCodec::readU64(bytes, MANIFEST_ENTRY_COUNT_OFFSET);
		result.encoded_bytes = LittleEndianCodec::readU64(bytes, MANIFEST_ENCODED_BYTES_OFFSET);
		std::vector<NativeBulkRunFragmentLocation> &references =
				result.level == 0U ? result.fragments : result.children;
		references.reserve(fragmentCount);
		for (size_t ordinal = 0U; ordinal < fragmentCount; ++ordinal) {
			const size_t offset = ordinal * RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES;
			NativeBulkRunFragmentLocation fragment{SegmentId(LittleEndianCodec::readU64(records,
																		 offset + MANIFEST_FRAGMENT_SEGMENT_ID_OFFSET)),
																		 Generation(LittleEndianCodec::readU64(records,
																		 offset + MANIFEST_FRAGMENT_GENERATION_OFFSET)), location.transaction_id};
			if (!fragment.isValid() || fragment.segment_id == location.segment_id) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable native bulk-run manifest has an invalid fragment reference");
			}
			references.push_back(fragment);
		}
		return result;
	}

	NativeBulkRunStore::NativeBulkRunStore(PhysicalSegmentAllocator &physical, RelocatableMetadataStore &metadata,
												 NativeBulkRunStoreOptions options) :
			physical_(physical), metadata_(metadata), options_(options) {
		if (!physical_.usesMetadataStore(metadata_) || !options_.first_slot.isValid() || !options_.last_slot.isValid() ||
			options_.first_slot > options_.last_slot || options_.maximum_fragment_bytes == 0U ||
			options_.maximum_fragment_bytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES ||
			options_.maximum_batch_fragments == 0U ||
			options_.maximum_manifest_depth == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run store requires finite physical and fragment bounds");
		}
	}

	NativeBulkRunFragmentLocation NativeBulkRunStore::write(uint64_t transactionId,
															 const NativeBulkRunFragment &fragment,
															 LogSequenceNumber checkpointLsn) {
		if (transactionId == 0U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run write requires a transaction owner and checkpoint LSN");
		}
		std::vector<std::byte> payload = NativeBulkRunFragmentCodec::encode(fragment);
		if (payload.size() > options_.maximum_fragment_bytes) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run fragment exceeds its configured byte bound");
		}
		return writeOpaquePayload(transactionId, std::move(payload), checkpointLsn);
	}

	std::vector<NativeBulkRunFragmentLocation> NativeBulkRunStore::writeBatch(
			uint64_t transactionId, std::span<const NativeBulkRunFragment> fragments,
			LogSequenceNumber checkpointLsn) {
		if (transactionId == 0U || fragments.empty() || fragments.size() > options_.maximum_batch_fragments ||
			!checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable native bulk-run batch requires an owner, fragments, and checkpoint LSN");
		}
		std::vector<std::vector<std::byte>> payloads;
		payloads.reserve(fragments.size());
		for (const NativeBulkRunFragment &fragment: fragments) {
			payloads.push_back(NativeBulkRunFragmentCodec::encode(fragment));
			if (payloads.back().size() > options_.maximum_fragment_bytes) {
				fail(FormatError::FERR_INVALID_STATE,
						 "relocatable native bulk-run batch contains an oversized fragment");
			}
		}
		return writeOpaquePayloadBatch(transactionId, std::move(payloads), checkpointLsn);
	}

	std::vector<NativeBulkRunFragmentLocation> NativeBulkRunStore::writeOpaquePayloadBatch(
			uint64_t transactionId, std::vector<std::vector<std::byte>> payloads,
			LogSequenceNumber checkpointLsn) {
		if (transactionId == 0U || payloads.empty() || payloads.size() > options_.maximum_batch_fragments ||
			!checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run payload batch requires bounded bytes, an owner, and a checkpoint LSN");
		}
		for (const std::vector<std::byte> &payload: payloads) {
			if (payload.empty() || payload.size() > options_.maximum_fragment_bytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable native bulk-run payload batch contains an invalid payload");
			}
		}
		std::vector<PhysicalSegmentAllocationRequest> requests;
		requests.reserve(payloads.size());
		const LogSequenceNumber allocationCheckpoint = nextCheckpointLsn(checkpointLsn);
		const std::vector<SegmentId> ids = metadata_.allocateLogicalSegmentIds(payloads.size(), checkpointLsn);
		if (ids.size() != payloads.size()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run payload batch did not reserve all Segment IDs");
		}
		for (size_t ordinal = 0U; ordinal < payloads.size(); ++ordinal) {
			PhysicalSegmentAllocationRequest request;
			request.segment_id = ids[ordinal];
			request.kind = SegmentKind::SGK_BULK_SCRATCH;
			request.owner_id = transactionId;
			request.generation = Generation(1U);
			request.payload = std::move(payloads[ordinal]);
			requests.push_back(std::move(request));
		}
		const std::optional<PhysicalSegmentAllocations> allocations = physical_.allocate(
			std::span<const PhysicalSegmentAllocationRequest>(requests), options_.first_slot, options_.last_slot,
			allocationCheckpoint);
		if (!allocations.has_value() || allocations->entries.size() != requests.size()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run payload batch exhausted its physical-slot range");
		}
		std::vector<NativeBulkRunFragmentLocation> locations;
		locations.reserve(allocations->entries.size());
		for (const CatalogEntry &entry: allocations->entries) {
			locations.push_back({entry.segment_id, entry.generation, transactionId});
		}
		return locations;
	}

	NativeBulkRunFragmentLocation NativeBulkRunStore::writeOpaquePayload(uint64_t transactionId,
																	std::vector<std::byte> payload,
																	LogSequenceNumber checkpointLsn) {
		if (transactionId == 0U || !checkpointLsn.isValid() || payload.empty() ||
			payload.size() > options_.maximum_fragment_bytes) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run payload requires bounded bytes, transaction owner, and checkpoint LSN");
		}
		std::vector<std::vector<std::byte>> payloads;
		payloads.push_back(std::move(payload));
		std::vector<NativeBulkRunFragmentLocation> locations =
				writeOpaquePayloadBatch(transactionId, std::move(payloads), checkpointLsn);
		return locations.front();
	}

	NativeBulkRunFragment NativeBulkRunStore::read(const MetadataSnapshotPin &snapshot,
															const NativeBulkRunFragmentLocation &location) const {
		if (!location.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run location is invalid");
		}
		return NativeBulkRunFragmentCodec::decode(readOpaquePayload(snapshot, location));
	}

	std::vector<std::byte> NativeBulkRunStore::readOpaquePayload(
			const MetadataSnapshotPin &snapshot, const NativeBulkRunFragmentLocation &location) const {
		if (!location.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run location is invalid");
		}
		const PhysicalSegmentContents contents = physical_.read(snapshot, location.segment_id);
		const uint8_t invalid = static_cast<uint8_t>(contents.entry.kind != SegmentKind::SGK_BULK_SCRATCH) |
								static_cast<uint8_t>(contents.entry.owner_id != location.transaction_id) |
								static_cast<uint8_t>(contents.entry.generation != location.generation);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run location does not match its durable scratch segment");
		}
		return contents.payload;
	}

	NativeBulkRunPayloadReader NativeBulkRunStore::openOpaquePayloadReader() const {
		auto snapshot = std::make_shared<MetadataSnapshotPin>(metadata_.openSnapshot());
		return [this, snapshot = std::move(snapshot)](const NativeBulkRunFragmentLocation &location) {
			return readOpaquePayload(*snapshot, location);
		};
	}

	NativeBulkRunManifest NativeBulkRunStore::writeManifest(uint64_t transactionId,
																			 std::span<const NativeBulkRunFragmentLocation> fragments,
																			 LogSequenceNumber checkpointLsn) {
		NativeBulkRunManifest manifest;
		manifest.fragments.assign(fragments.begin(), fragments.end());
		if (manifest.fragments.empty()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run manifest requires at least one fragment");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		std::optional<std::vector<std::byte>> lastKey;
		for (const NativeBulkRunFragmentLocation &fragment: manifest.fragments) {
			if (fragment.transaction_id != transactionId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run manifest cannot reference a foreign transaction fragment");
			}
			const NativeBulkRunFragment decoded = read(snapshot, fragment);
			for (const BTreePageEntry &entry: decoded.entries) {
				if (lastKey.has_value() && !lessKey(*lastKey, entry.key)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable native bulk-run manifest fragments are not globally ordered");
				}
				lastKey = entry.key;
			}
			if (manifest.entry_count > std::numeric_limits<uint64_t>::max() - decoded.entries.size()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable native bulk-run manifest entry count overflows");
			}
			const std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(decoded);
			if (manifest.encoded_bytes > std::numeric_limits<uint64_t>::max() - encoded.size()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable native bulk-run manifest byte count overflows");
			}
			manifest.entry_count += decoded.entries.size();
			manifest.encoded_bytes += encoded.size();
		}
		snapshot.release();
		const std::vector<std::byte> payload = NativeBulkRunManifestCodec::encode(manifest);
		manifest.location = writeOpaquePayload(transactionId, payload, checkpointLsn);
		return manifest;
	}

	NativeBulkRunManifest NativeBulkRunStore::writeManifestParent(
			uint64_t transactionId, std::span<const NativeBulkRunFragmentLocation> children,
			LogSequenceNumber checkpointLsn) {
		if (children.empty()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run parent manifest requires at least one child");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		NativeBulkRunManifest manifest;
		manifest.children.assign(children.begin(), children.end());
		std::optional<std::vector<std::byte>> lastKey;
		std::optional<uint16_t> childLevel;
		for (const NativeBulkRunFragmentLocation &child: manifest.children) {
			if (child.transaction_id != transactionId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run parent cannot reference a foreign transaction manifest");
			}
			const NativeBulkRunManifest decoded = readManifest(snapshot, child);
			if (!childLevel.has_value()) {
				childLevel = decoded.level;
				if (static_cast<uint32_t>(*childLevel) + 1U >= options_.maximum_manifest_depth) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable native bulk-run parent exceeds its configured manifest depth");
				}
				manifest.level = static_cast<uint16_t>(*childLevel + 1U);
			} else if (decoded.level != *childLevel) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run parent requires same-level children");
			}
			NativeBulkRunManifest firstLeaf = decoded;
			while (firstLeaf.level != 0U) {
				firstLeaf = readManifest(snapshot, firstLeaf.children.front());
			}
			const NativeBulkRunFragment first = read(snapshot, firstLeaf.fragments.front());
			if (lastKey.has_value() && !lessKey(*lastKey, first.entries.front().key)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run parent manifests are not globally ordered");
			}
			NativeBulkRunManifest lastLeaf = decoded;
			while (lastLeaf.level != 0U) {
				lastLeaf = readManifest(snapshot, lastLeaf.children.back());
			}
			const NativeBulkRunFragment last = read(snapshot, lastLeaf.fragments.back());
			lastKey = last.entries.back().key;
			if (manifest.entry_count > std::numeric_limits<uint64_t>::max() - decoded.entry_count ||
				manifest.encoded_bytes > std::numeric_limits<uint64_t>::max() - decoded.encoded_bytes) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable native bulk-run parent aggregate counters overflow");
			}
			manifest.entry_count += decoded.entry_count;
			manifest.encoded_bytes += decoded.encoded_bytes;
		}
		manifest.location = writeOpaquePayload(transactionId, NativeBulkRunManifestCodec::encode(manifest), checkpointLsn);
		return manifest;
	}

	NativeBulkRunManifest NativeBulkRunStore::readManifest(const MetadataSnapshotPin &snapshot,
																																												 const NativeBulkRunFragmentLocation &location) const {
		NativeBulkRunManifest manifest = NativeBulkRunManifestCodec::decode(readOpaquePayload(snapshot, location), location);
		if (manifest.level >= options_.maximum_manifest_depth) {
			fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run manifest exceeds its configured depth");
		}
		return manifest;
	}

	OrderedBTreeEntrySource
	NativeBulkRunStore::orderedSource(const NativeBulkRunFragmentLocation &manifestLocation) const {
		if (!manifestLocation.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable native bulk-run source requires a valid manifest location");
		}
		return [this, manifestLocation] {
			auto snapshot = std::make_shared<MetadataSnapshotPin>(metadata_.openSnapshot());
			const NativeBulkRunPayloadReader reader = [this, snapshot](
														 const NativeBulkRunFragmentLocation &location) mutable {
				return readOpaquePayload(*snapshot, location);
			};
			NativeBulkRunManifest manifest = NativeBulkRunManifestCodec::decode(reader(manifestLocation), manifestLocation);
			if (manifest.level >= options_.maximum_manifest_depth) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run manifest exceeds its configured depth");
			}
			auto cursor = std::make_shared<NativeBulkRunManifestCursor>(
					std::move(manifest), options_.maximum_manifest_depth, reader);
			return [cursor] { return cursor->next(); };
		};
	}

	OrderedBTreeEntrySource NativeBulkRunStore::orderedSourceFromPayloadReader(
			const NativeBulkRunFragmentLocation &manifestLocation, uint16_t maximumManifestDepth,
			NativeBulkRunPayloadReader payloadReader) {
		if (!manifestLocation.isValid() || maximumManifestDepth == 0U || !payloadReader) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run candidate source requires a valid manifest, depth, and reader");
		}
		return [manifestLocation, maximumManifestDepth, payloadReader = std::move(payloadReader)] {
			NativeBulkRunManifest manifest =
					NativeBulkRunManifestCodec::decode(payloadReader(manifestLocation), manifestLocation);
			if (manifest.level >= maximumManifestDepth) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run candidate manifest exceeds its configured depth");
			}
			auto cursor = std::make_shared<NativeBulkRunManifestCursor>(
					std::move(manifest), maximumManifestDepth, payloadReader);
			return [cursor] { return cursor->next(); };
		};
	}

	void NativeBulkRunStore::releaseManifest(const NativeBulkRunFragmentLocation &manifestLocation,
			LogSequenceNumber checkpointLsn) {
		if (!manifestLocation.isValid() || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run manifest release requires a valid location and checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		NativeBulkRunManifest manifest = readManifest(snapshot, manifestLocation);
		std::vector<NativeBulkRunFragmentLocation> locations;
		std::vector<NativeBulkRunFragmentLocation> manifests;
		std::vector<std::pair<NativeBulkRunFragmentLocation, uint16_t>> current{{manifestLocation, manifest.level}};
		std::set<std::pair<uint64_t, uint64_t>> visitedManifests;
		std::set<std::pair<uint64_t, uint64_t>> visitedFragments;
		while (!current.empty()) {
			std::vector<std::pair<NativeBulkRunFragmentLocation, uint16_t>> next;
			for (const auto &[location, expectedLevel]: current) {
				const auto identity = std::pair{location.segment_id.value(), location.generation.value()};
				if (!visitedManifests.insert(identity).second) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk-run manifest release found a duplicate or cyclic manifest");
				}
				const NativeBulkRunManifest currentManifest = location == manifestLocation
							? manifest
							: readManifest(snapshot, location);
				if (currentManifest.level != expectedLevel) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk-run manifest release found an inconsistent child level");
				}
				manifests.push_back(location);
				if (currentManifest.level == 0U) {
					for (const NativeBulkRunFragmentLocation &fragment: currentManifest.fragments) {
						const auto fragmentIdentity =
								std::pair{fragment.segment_id.value(), fragment.generation.value()};
						if (!visitedFragments.insert(fragmentIdentity).second) {
							fail(FormatError::FERR_INVALID_STATE,
									 "relocatable native bulk-run manifest release found a duplicate fragment");
						}
						locations.push_back(fragment);
					}
				} else {
					for (const NativeBulkRunFragmentLocation &child: currentManifest.children) {
						next.push_back({child, static_cast<uint16_t>(currentManifest.level - 1U)});
					}
				}
			}
			current = std::move(next);
		}
		snapshot.release();
		LogSequenceNumber nextLsn = checkpointLsn;
		locations.insert(locations.end(), manifests.begin(), manifests.end());
		// Preflight the complete checkpoint sequence before the first release; a
		// deep manifest must not be partially reclaimed on arithmetic overflow.
		for (size_t ordinal = 0U; ordinal + 1U < locations.size(); ++ordinal) {
			nextLsn = ::graph::storage::advanceCheckpointLsn(nextLsn, 2U);
		}
		nextLsn = checkpointLsn;
		for (size_t ordinal = 0U; ordinal < locations.size(); ++ordinal) {
			release(locations[ordinal], nextLsn);
			if (ordinal + 1U != locations.size()) {
				nextLsn = ::graph::storage::advanceCheckpointLsn(nextLsn, 2U);
			}
		}
	}

	size_t NativeBulkRunStore::maximumManifestReferences() const {
		return (options_.maximum_fragment_bytes - RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) /
			   RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES;
	}

	size_t NativeBulkRunStore::maximumBatchFragments() const { return options_.maximum_batch_fragments; }

	uint16_t NativeBulkRunStore::maximumManifestDepth() const { return options_.maximum_manifest_depth; }

	size_t NativeBulkRunStore::maximumFragmentBytes() const { return options_.maximum_fragment_bytes; }

	NativeBulkRunStreamingWriter::NativeBulkRunStreamingWriter(NativeBulkRunStore &store, uint64_t transactionId,
																								  NativeBulkRunStreamingOptions options,
																								  LogSequenceNumber firstCheckpointLsn) :
			store_(store), transaction_id_(transactionId), options_(options), next_checkpoint_lsn_(firstCheckpointLsn) {
		if (transaction_id_ == 0U || options_.maximum_buffered_bytes == 0U || options_.maximum_entries == 0U ||
			!next_checkpoint_lsn_.isValid() || options_.maximum_buffered_bytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES ||
			store_.maximumFragmentBytes() < RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			options_.maximum_buffered_bytes > store_.maximumFragmentBytes() - RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			store_.maximumManifestReferences() < 2U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run streaming writer requires finite entry, buffer, and manifest bounds");
		}
	}

	void NativeBulkRunStreamingWriter::advanceCheckpointLsn() {
		// A scratch allocation reserves its SegmentId at this LSN and publishes
		// the physical envelope at the following LSN, so the next independent
		// allocation must start after both metadata publications.
		next_checkpoint_lsn_ = ::graph::storage::advanceCheckpointLsn(next_checkpoint_lsn_, 2U);
	}

	size_t NativeBulkRunStreamingWriter::maximumManifestReferences() const { return store_.maximumManifestReferences(); }

	void NativeBulkRunStreamingWriter::flushFragment() {
		if (buffered_.entries.empty()) {
			return;
		}
		pending_fragment_batch_.push_back(std::move(buffered_));
		buffered_ = {};
		buffered_bytes_ = 0U;
		if (pending_fragment_batch_.size() == store_.maximumBatchFragments()) {
			const std::vector<NativeBulkRunFragmentLocation> locations =
					store_.writeBatch(transaction_id_, pending_fragment_batch_, next_checkpoint_lsn_);
			pending_fragments_.insert(pending_fragments_.end(), locations.begin(), locations.end());
			pending_fragment_batch_.clear();
			advanceCheckpointLsn();
			sealLeafManifest();
		}
	}

	void NativeBulkRunStreamingWriter::appendManifestAtLevel(NativeBulkRunFragmentLocation location, size_t level) {
		if (level >= store_.maximumManifestDepth()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run streaming writer exceeded its configured manifest depth");
		}
		if (manifest_levels_.size() <= level) {
			manifest_levels_.resize(level + 1U);
		}
		manifest_levels_[level].push_back(location);
		if (manifest_levels_[level].size() == maximumManifestReferences()) {
			sealParentManifest(level);
		}
	}

	void NativeBulkRunStreamingWriter::sealLeafManifest() {
		if (!pending_fragment_batch_.empty()) {
			const std::vector<NativeBulkRunFragmentLocation> locations =
					store_.writeBatch(transaction_id_, pending_fragment_batch_, next_checkpoint_lsn_);
			pending_fragments_.insert(pending_fragments_.end(), locations.begin(), locations.end());
			pending_fragment_batch_.clear();
			advanceCheckpointLsn();
		}
		// Every caller reaches this helper after staging a nonempty fragment batch.
		const NativeBulkRunManifest manifest = store_.writeManifest(transaction_id_, pending_fragments_, next_checkpoint_lsn_);
		advanceCheckpointLsn();
		pending_fragments_.clear();
		appendManifestAtLevel(manifest.location, 0U);
	}

	void NativeBulkRunStreamingWriter::sealParentManifest(size_t level) {
		// Callers either just filled this level or selected it as a nonempty root.
		const NativeBulkRunManifest manifest =
				store_.writeManifestParent(transaction_id_, manifest_levels_[level], next_checkpoint_lsn_);
		advanceCheckpointLsn();
		manifest_levels_[level].clear();
		appendManifestAtLevel(manifest.location, level + 1U);
	}

	void NativeBulkRunStreamingWriter::add(BTreePageEntry entry) {
		if (finished_ || entry_count_ == options_.maximum_entries) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run streaming writer cannot accept more entries");
		}
		validateEntry(entry);
		if (last_key_.has_value() && !lessKey(*last_key_, entry.key)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run streaming writer requires strictly ordered entries");
		}
		const size_t encodedSize = BTreePageEntryCodec::encodedSize(entry);
		if (encodedSize > std::numeric_limits<size_t>::max() - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "relocatable native bulk-run streaming entry byte count overflowed");
		}
		const size_t encodedBytes = encodedSize + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES;
		if (encodedBytes > options_.maximum_buffered_bytes) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run entry cannot fit in its configured streaming buffer");
		}
		if (buffered_bytes_ > options_.maximum_buffered_bytes - encodedBytes) {
			flushFragment();
		}
		buffered_.entries.push_back(std::move(entry));
		last_key_ = buffered_.entries.back().key;
		buffered_bytes_ += encodedBytes;
		++entry_count_;
	}

	NativeBulkRunManifest NativeBulkRunStreamingWriter::finish() {
		if (finished_ || entry_count_ == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable native bulk-run streaming writer can finish one nonempty run only once");
		}
		flushFragment();
		sealLeafManifest();
		for (;;) {
			size_t lastNonEmptyLevel = 0U;
			size_t totalRoots = 0U;
			for (size_t level = 0U; level < manifest_levels_.size(); ++level) {
				if (!manifest_levels_[level].empty()) {
					lastNonEmptyLevel = level;
					totalRoots += manifest_levels_[level].size();
				}
			}
			if (totalRoots == 1U) {
				MetadataSnapshotPin snapshot = store_.metadata_.openSnapshot();
				NativeBulkRunManifest result =
						store_.readManifest(snapshot, manifest_levels_[lastNonEmptyLevel].front());
				snapshot.release();
				finished_ = true;
				return result;
			}
			bool progressed = false;
			for (size_t level = 0U; level <= lastNonEmptyLevel; ++level) {
				if (manifest_levels_[level].empty()) {
					continue;
				}
				if (manifest_levels_[level].size() > 1U || manifest_levels_[level].size() >= maximumManifestReferences()) {
					sealParentManifest(level);
					progressed = true;
					break;
				}
				if (manifest_levels_[level].size() == 1U && level < lastNonEmptyLevel) {
					const NativeBulkRunFragmentLocation location = manifest_levels_[level].front();
					const NativeBulkRunManifest parent =
							store_.writeManifestParent(transaction_id_, std::array{location}, next_checkpoint_lsn_);
					advanceCheckpointLsn();
					manifest_levels_[level].clear();
					appendManifestAtLevel(parent.location, level + 1U);
					progressed = true;
					break;
				}
			}
			if (!progressed) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run streaming writer could not seal one root manifest");
			}
		}
	}

	OrderedBTreeEntrySource NativeBulkRunMerger::orderedSource(
			std::span<const OrderedBTreeEntrySource> inputs, size_t maximumOpenRuns, size_t maximumEntries) {
		if (inputs.empty() || inputs.size() > maximumOpenRuns || maximumOpenRuns == 0U || maximumEntries == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable bounded bulk-run merge source requires finite input, fan-in, and entry limits");
		}
		for (const OrderedBTreeEntrySource &input: inputs) {
			if (!input) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable bounded bulk-run merge source received an empty input cursor factory");
			}
		}
		std::vector<OrderedBTreeEntrySource> ownedInputs(inputs.begin(), inputs.end());
		return [inputs = std::move(ownedInputs), maximumEntries] {
			auto state = std::make_shared<NativeBulkRunMergedSourceState>();
			state->maximum_entries = maximumEntries;
			state->cursors.reserve(inputs.size());
			state->previous_input_keys.resize(inputs.size());
			state->ready.reserve(inputs.size());
			for (const OrderedBTreeEntrySource &input: inputs) {
				state->cursors.push_back(input());
				if (!state->cursors.back()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable bounded bulk-run merge source factory returned an empty cursor");
				}
			}
			for (size_t ordinal = 0U; ordinal < state->cursors.size(); ++ordinal) {
				state->addNext(ordinal);
			}
			return [state] { return state->next(); };
		};
	}

	NativeBulkRunManifest NativeBulkRunMerger::merge(NativeBulkRunStore &store, uint64_t transactionId,
			std::span<const NativeBulkRunFragmentLocation> inputs, NativeBulkRunMergeOptions options,
			LogSequenceNumber firstCheckpointLsn) {
		if (transactionId == 0U || inputs.size() < 2U || options.maximum_open_runs < 2U ||
			inputs.size() > options.maximum_open_runs || options.output.maximum_buffered_bytes == 0U ||
			options.output.maximum_entries == 0U || !firstCheckpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run merge requires a bounded nonempty fan-in and output budget");
		}
		std::vector<OrderedBTreeEntryCursor> cursors;
		cursors.reserve(inputs.size());
		for (const NativeBulkRunFragmentLocation &input: inputs) {
			if (input.transaction_id != transactionId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run merge cannot read a foreign transaction manifest");
			}
			cursors.push_back(store.orderedSource(input)());
		}
		std::vector<NativeBulkRunMergeItem> queue;
		queue.reserve(cursors.size());
		for (size_t ordinal = 0U; ordinal < cursors.size(); ++ordinal) {
			if (std::optional<BTreePageEntry> entry = cursors[ordinal](); entry.has_value()) {
				queue.push_back({ordinal, std::move(*entry)});
				std::push_heap(queue.begin(), queue.end(), NativeBulkRunMergeItemGreater{});
			}
		}
		NativeBulkRunStreamingWriter writer(store, transactionId, options.output, firstCheckpointLsn);
		std::optional<std::vector<std::byte>> lastKey;
		while (!queue.empty()) {
			std::pop_heap(queue.begin(), queue.end(), NativeBulkRunMergeItemGreater{});
			NativeBulkRunMergeItem next = std::move(queue.back());
			queue.pop_back();
			if (lastKey.has_value() && !lessKey(*lastKey, next.entry.key)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk-run merge rejects duplicate or overlapping input keys");
			}
			lastKey = next.entry.key;
			writer.add(std::move(next.entry));
			if (std::optional<BTreePageEntry> replacement = cursors[next.cursor_ordinal](); replacement.has_value()) {
				queue.push_back({next.cursor_ordinal, std::move(*replacement)});
				std::push_heap(queue.begin(), queue.end(), NativeBulkRunMergeItemGreater{});
			}
		}
		return writer.finish();
	}

	void NativeBulkRunStore::release(const NativeBulkRunFragmentLocation &location, LogSequenceNumber checkpointLsn) {
		const std::array locations{location};
		releaseBatch(locations, checkpointLsn);
	}

	void NativeBulkRunStore::releaseBatch(std::span<const NativeBulkRunFragmentLocation> locations,
			LogSequenceNumber checkpointLsn) {
		if (locations.empty() || locations.size() > options_.maximum_batch_fragments || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
					"relocatable native bulk-run release requires bounded locations and a checkpoint LSN");
		}
		// Validate reclamation before retiring anything so overflow cannot leave a
		// partially published release behind.
		const LogSequenceNumber reclaimCheckpointLsn = nextCheckpointLsn(checkpointLsn);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		std::vector<CatalogSegmentRetirementRequest> requests;
		requests.reserve(locations.size());
		std::set<std::pair<uint64_t, uint64_t>> seen;
		try {
			for (const NativeBulkRunFragmentLocation &location: locations) {
				if (!location.isValid() ||
					!seen.insert({location.segment_id.value(), location.generation.value()}).second) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable native bulk-run release has an invalid or duplicate location");
				}
				const CatalogEntry entry = metadata_.findCatalog(snapshot, location.segment_id);
				const uint8_t invalid = static_cast<uint8_t>(entry.kind != SegmentKind::SGK_BULK_SCRATCH) |
										static_cast<uint8_t>(entry.owner_id != location.transaction_id) |
										static_cast<uint8_t>(entry.generation != location.generation) |
										static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED);
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable native bulk-run release has a stale or foreign scratch location");
				}
				requests.push_back({entry.segment_id, entry.generation});
			}
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}
		static_cast<void>(metadata_.retireCatalogSegments(requests, checkpointLsn));
		static_cast<void>(metadata_.reclaimRetiredPhysicalSlots(reclaimCheckpointLsn));
	}

	void NativeBulkRunStore::reclaimUnpublishedOnOpen(LogSequenceNumber checkpointLsn) {
		if (!checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native bulk-run recovery requires a checkpoint LSN");
		}
		LogSequenceNumber nextLsn = checkpointLsn;
		for (;;) {
			MetadataSnapshotPin snapshot = metadata_.openSnapshot();
			std::vector<NativeBulkRunFragmentLocation> candidates;
			candidates.reserve(options_.maximum_batch_fragments);
			metadata_.visitCatalogEntries(snapshot, [&candidates, this](const CatalogEntry &entry) {
				if (candidates.size() < options_.maximum_batch_fragments &&
					entry.kind == SegmentKind::SGK_BULK_SCRATCH &&
					entry.state == SegmentState::SGS_SEALED) {
					candidates.push_back({entry.segment_id, entry.generation, entry.owner_id});
				}
			});
			snapshot.release();
			if (candidates.empty()) {
				return;
			}
			releaseBatch(candidates, nextLsn);
			nextLsn = advanceCheckpointLsn(nextLsn, 2U);
		}
	}


} // namespace graph::storage
