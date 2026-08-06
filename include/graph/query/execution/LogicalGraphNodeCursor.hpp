/**
 * @file LogicalGraphNodeCursor.hpp
 * @brief Snapshot-pinned pull cursor over bounded logical Node pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::query::execution {

	struct LogicalGraphNodeCursorOptions {
		size_t page_records = 0U;
		size_t maximum_metadata_pages_per_page = 0U;

		constexpr bool operator==(const LogicalGraphNodeCursorOptions &) const = default;
	};

	// This cursor preserves one backend snapshot while presenting a pull-based
	// source to future physical operators. It never restarts a directory scan or
	// accumulates more than one backend-provided page.
	class LogicalGraphNodeCursor final {
	public:
		LogicalGraphNodeCursor(const storage::LogicalGraphReadBackend &backend, LogicalGraphNodeCursorOptions options);

		void open();
		[[nodiscard]] std::optional<storage::LogicalGraphNodePage> next();
		void close() noexcept;
		[[nodiscard]] bool isOpen() const noexcept { return snapshot_ != nullptr; }

	private:
		const storage::LogicalGraphReadBackend &backend_;
		LogicalGraphNodeCursorOptions options_;
		std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot_;
		uint64_t after_node_id_ = 0U;
		bool exhausted_ = false;
	};

} // namespace graph::query::execution
