/**
 * @file ExternalBTreeEntrySorter.hpp
 * @brief Bounded external sort for immutable relocatable B+Tree build entries.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"

namespace graph::storage {
	namespace detail {
		struct ExternalBTreeEntrySorterState;
	}

	struct ExternalBTreeEntrySorterOptions {
		uint64_t maximum_entries = 0;
		size_t maximum_run_bytes = 0;
		size_t maximum_open_runs = 0;
	};

	// Builds independently rewindable, bytewise-sorted entry cursors without
	// keeping the whole index input in memory. Temporary runs use anonymous
	// `tmpfile` handles and explicit little-endian record framing, so no caller
	// supplied path is opened or trusted.
	class ExternalBTreeEntrySorter final {
	public:
		explicit ExternalBTreeEntrySorter(ExternalBTreeEntrySorterOptions options);
		ExternalBTreeEntrySorter(const ExternalBTreeEntrySorter &) = delete;
		ExternalBTreeEntrySorter &operator=(const ExternalBTreeEntrySorter &) = delete;
		ExternalBTreeEntrySorter(ExternalBTreeEntrySorter &&) noexcept;
		ExternalBTreeEntrySorter &operator=(ExternalBTreeEntrySorter &&) noexcept;
		~ExternalBTreeEntrySorter();

		void add(BTreePageEntry entry);
		// Freezes all runs and returns a source suitable for the two independent
		// passes required by BulkBTreeBuilder. Cursors are intentionally serial:
		// a second pass must start only after the first cursor is destroyed.
		[[nodiscard]] OrderedBTreeEntrySource finish();

	private:
		std::shared_ptr<detail::ExternalBTreeEntrySorterState> state_;
	};

} // namespace graph::storage
