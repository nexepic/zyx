/**
 * @file ExternalBTreeEntrySorter.cpp
 * @brief Bounded anonymous-run external sort implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/ExternalBTreeEntrySorter.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAXIMUM_RUN_ENTRY_BYTES = RSS_BTREE_ENTRY_HEADER_BYTES + RSS_INDEX_KEY_MAX_BYTES;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		void validateOptions(const ExternalBTreeEntrySorterOptions &options) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_entries == 0U) |
									static_cast<uint8_t>(options.maximum_run_bytes < RSS_BTREE_ENTRY_HEADER_BYTES) |
									static_cast<uint8_t>(options.maximum_open_runs < 2U) |
									static_cast<uint8_t>(options.maximum_open_runs > 64U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable external B+Tree sort requires entry, run-byte, and open-run bounds");
			}
		}

		class TemporaryRun final {
		public:
			TemporaryRun() : file_(std::tmpfile()) {
				if (file_ == nullptr) { // ZYX_COV_EXCL_LINE: OS temporary-file allocation failure is not injectable here.
					fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sort could not create an anonymous run");
				}
			}
			TemporaryRun(const TemporaryRun &) = delete;
			TemporaryRun &operator=(const TemporaryRun &) = delete;
			~TemporaryRun() {
				if (file_ != nullptr) { // ZYX_COV_EXCL_LINE: construction either returns a valid tmpfile or throws.
					std::fclose(file_);
				}
			}

			[[nodiscard]] std::FILE *file() const { return file_; }

		private:
			std::FILE *file_ = nullptr;
		};

		void writeAll(std::FILE *file, const void *bytes, size_t length) {
			if (std::fwrite(bytes, 1U, length, file) != length) { // ZYX_COV_EXCL_LINE: anonymous temporary-run I/O failure.
				fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sort could not write a temporary run");
			}
		}

		void writeEntry(std::FILE *file, const BTreePageEntry &entry) {
			const std::vector<std::byte> encoded = BTreePageEntryCodec::encode(entry);
			if (encoded.size() > MAXIMUM_RUN_ENTRY_BYTES) { // ZYX_COV_EXCL_LINE: add() rejects this before run writing.
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable external B+Tree sort entry exceeds the configured index-key framing bound");
			}
			std::array<std::byte, sizeof(uint32_t)> length{};
			LittleEndianCodec::writeU32(length, 0, static_cast<uint32_t>(encoded.size()));
			writeAll(file, length.data(), length.size());
			writeAll(file, encoded.data(), encoded.size());
		}

		class RunReader final {
		public:
			explicit RunReader(std::FILE *file) : file_(file) {
				if (file_ == nullptr) { // ZYX_COV_EXCL_LINE: only TemporaryRun supplies readers.
					fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sort reader has no temporary run");
				}
				std::clearerr(file_);
				std::rewind(file_);
			}

			[[nodiscard]] std::optional<BTreePageEntry> next() {
				std::array<std::byte, sizeof(uint32_t)> length{};
				const size_t readLength = std::fread(length.data(), 1U, length.size(), file_);
				if (readLength == 0U) {
					if (std::feof(file_) != 0) { // ZYX_COV_EXCL_LINE: private run reads reach EOF or raise an OS I/O error.
						return std::nullopt;
					}
					fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sort could not read a temporary run");
				}
				if (readLength != length.size()) { // ZYX_COV_EXCL_LINE: temporary runs are private, complete framed writes.
					fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable external B+Tree sort run length is truncated");
				}
				const uint32_t entryBytes = LittleEndianCodec::readU32(length, 0);
				if (entryBytes < RSS_BTREE_ENTRY_HEADER_BYTES || entryBytes > MAXIMUM_RUN_ENTRY_BYTES) { // ZYX_COV_EXCL_LINE:
					// private run framing is bounded by writeEntry and has no untrusted writer.
					fail(FormatError::FERR_INVALID_HEADER, "relocatable external B+Tree sort run has an invalid entry length");
				}
				std::vector<std::byte> bytes(entryBytes);
				if (std::fread(bytes.data(), 1U, bytes.size(), file_) != bytes.size()) { // ZYX_COV_EXCL_LINE: private full write.
					fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable external B+Tree sort run entry is truncated");
				}
				return BTreePageEntryCodec::decode(bytes);
			}

		private:
			std::FILE *file_ = nullptr;
		};

		struct MergeItem {
			size_t run_ordinal = 0;
			BTreePageEntry entry;
		};

		struct MergeItemGreater {
			[[nodiscard]] bool operator()(const MergeItem &left, const MergeItem &right) const {
				if (lessKey(right.entry.key, left.entry.key)) {
					return true;
				}
				if (lessKey(left.entry.key, right.entry.key)) {
					return false;
				}
				return left.run_ordinal > right.run_ordinal;
			}
		};

		class MergeCursor final {
		public:
			using CursorClosed = std::function<void()>;

			MergeCursor(std::span<const std::unique_ptr<TemporaryRun>> runs, CursorClosed closed) : closed_(std::move(closed)) {
				readers_.reserve(runs.size());
				for (const std::unique_ptr<TemporaryRun> &run: runs) {
					readers_.emplace_back(run->file());
				}
				for (size_t runOrdinal = 0; runOrdinal < readers_.size(); ++runOrdinal) {
				if (std::optional<BTreePageEntry> entry = readers_[runOrdinal].next(); entry.has_value()) { // ZYX_COV_EXCL_LINE:
					// every persisted run is created from at least one pending entry.
						queue_.push({runOrdinal, std::move(*entry)});
					}
				}
			}
			MergeCursor(const MergeCursor &) = delete;
			MergeCursor &operator=(const MergeCursor &) = delete;
			~MergeCursor() {
				if (closed_) { // ZYX_COV_EXCL_LINE: every sorter-owned cursor supplies its state-release callback.
					closed_();
				}
			}

			[[nodiscard]] std::optional<BTreePageEntry> next() {
				if (queue_.empty()) {
					return std::nullopt;
				}
				MergeItem next = queue_.top();
				queue_.pop();
				if (std::optional<BTreePageEntry> replacement = readers_[next.run_ordinal].next(); replacement.has_value()) {
					queue_.push({next.run_ordinal, std::move(*replacement)});
				}
				return next.entry;
			}

		private:
			std::vector<RunReader> readers_;
			std::priority_queue<MergeItem, std::vector<MergeItem>, MergeItemGreater> queue_;
			CursorClosed closed_;
		};

	} // namespace

	struct detail::ExternalBTreeEntrySorterState final {
		explicit ExternalBTreeEntrySorterState(ExternalBTreeEntrySorterOptions options) : options(std::move(options)) {}

		ExternalBTreeEntrySorterOptions options;
		std::vector<BTreePageEntry> pending;
		size_t pending_bytes = 0;
		uint64_t entry_count = 0;
		std::vector<std::unique_ptr<TemporaryRun>> runs;
		bool finished = false;
		bool cursor_open = false;
	};

	namespace {

		void flushPending(detail::ExternalBTreeEntrySorterState &state) {
			if (state.pending.empty()) {
				return;
			}
			std::ranges::sort(state.pending, {}, &BTreePageEntry::key);
			auto run = std::make_unique<TemporaryRun>();
			for (const BTreePageEntry &entry: state.pending) {
				writeEntry(run->file(), entry);
			}
			if (std::fflush(run->file()) != 0) { // ZYX_COV_EXCL_LINE: anonymous temporary-run I/O failure.
				fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sort could not flush a temporary run");
			}
			state.runs.push_back(std::move(run));
			state.pending.clear();
			state.pending_bytes = 0;
		}

		[[nodiscard]] std::unique_ptr<TemporaryRun>
		mergeRuns(std::span<const std::unique_ptr<TemporaryRun>> runs) {
			if (runs.size() < 2U) { // ZYX_COV_EXCL_LINE: State merges only a full multi-run group.
				fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sort merge requires multiple runs");
			}
			std::vector<RunReader> readers;
			readers.reserve(runs.size());
			for (const std::unique_ptr<TemporaryRun> &run: runs) {
				readers.emplace_back(run->file());
			}
			std::priority_queue<MergeItem, std::vector<MergeItem>, MergeItemGreater> queue;
			for (size_t runOrdinal = 0; runOrdinal < readers.size(); ++runOrdinal) {
				if (std::optional<BTreePageEntry> entry = readers[runOrdinal].next(); entry.has_value()) { // ZYX_COV_EXCL_LINE:
					// reduction merges only nonempty runs emitted by flushPending.
					queue.push({runOrdinal, std::move(*entry)});
				}
			}
			auto output = std::make_unique<TemporaryRun>();
			while (!queue.empty()) {
				MergeItem next = queue.top();
				queue.pop();
				writeEntry(output->file(), next.entry);
				if (std::optional<BTreePageEntry> replacement = readers[next.run_ordinal].next(); replacement.has_value()) {
					queue.push({next.run_ordinal, std::move(*replacement)});
				}
			}
			if (std::fflush(output->file()) != 0) { // ZYX_COV_EXCL_LINE: anonymous temporary-run I/O failure.
				fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sort could not flush a merged run");
			}
			return output;
		}

		void reduceRuns(detail::ExternalBTreeEntrySorterState &state) {
			while (state.runs.size() > state.options.maximum_open_runs) {
				const size_t groupSize = std::min(state.runs.size(), state.options.maximum_open_runs);
				std::unique_ptr<TemporaryRun> merged = mergeRuns(
						std::span<const std::unique_ptr<TemporaryRun>>(state.runs.data(), groupSize));
				state.runs.erase(state.runs.begin(), state.runs.begin() + static_cast<std::ptrdiff_t>(groupSize));
				state.runs.push_back(std::move(merged));
			}
		}

	} // namespace

	ExternalBTreeEntrySorter::ExternalBTreeEntrySorter(ExternalBTreeEntrySorterOptions options) {
		validateOptions(options);
		state_ = std::make_shared<detail::ExternalBTreeEntrySorterState>(std::move(options));
	}

	ExternalBTreeEntrySorter::ExternalBTreeEntrySorter(ExternalBTreeEntrySorter &&) noexcept = default;

	ExternalBTreeEntrySorter &ExternalBTreeEntrySorter::operator=(ExternalBTreeEntrySorter &&) noexcept = default;

	ExternalBTreeEntrySorter::~ExternalBTreeEntrySorter() = default;

	void ExternalBTreeEntrySorter::add(BTreePageEntry entry) {
		if (!state_ || state_->finished) {
			fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sorter cannot accept entries after finishing");
		}
		if (state_->entry_count == state_->options.maximum_entries) {
			fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sorter exceeded its configured entry bound");
		}
		const std::vector<std::byte> encoded = BTreePageEntryCodec::encode(entry);
		if (encoded.size() > MAXIMUM_RUN_ENTRY_BYTES || encoded.size() > state_->options.maximum_run_bytes) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable external B+Tree sorter entry cannot fit in the configured in-memory run bound");
		}
		if (state_->pending_bytes > state_->options.maximum_run_bytes - encoded.size()) {
			flushPending(*state_);
		}
		state_->pending.push_back(std::move(entry));
		state_->pending_bytes += encoded.size();
		++state_->entry_count;
	}

	OrderedBTreeEntrySource ExternalBTreeEntrySorter::finish() {
		if (!state_) {
			fail(FormatError::FERR_INVALID_STATE, "relocatable external B+Tree sorter has no state");
		}
		if (!state_->finished) {
			flushPending(*state_);
			reduceRuns(*state_);
			state_->finished = true;
		}
		const std::shared_ptr<detail::ExternalBTreeEntrySorterState> state = state_;
		return [state] {
			if (state->cursor_open) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable external B+Tree sorter permits only one active rewindable source cursor");
			}
			state->cursor_open = true;
			auto cursor = std::make_shared<MergeCursor>(state->runs, [state] { state->cursor_open = false; });
			return [cursor]() { return cursor->next(); };
		};
	}

} // namespace graph::storage
