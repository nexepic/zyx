/**
 * @file LogicalGraphAdjacencyCursor.hpp
 * @brief Snapshot-pinned pull cursor over resumable logical adjacency pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::query::execution {

	struct LogicalGraphAdjacencyCursorOptions {
		size_t page_records = 0U;
		size_t maximum_total_edges = 0U;

		constexpr bool operator==(const LogicalGraphAdjacencyCursorOptions &) const = default;
	};

	// The cursor holds one read snapshot and resumes at the exact next logical
	// Edge. Backends validate the continuation link; the cursor additionally
	// enforces a total bound so a malformed cycle cannot consume unbounded work.
	class LogicalGraphAdjacencyCursor final {
	public:
		LogicalGraphAdjacencyCursor(const storage::LogicalGraphReadBackend &backend, uint64_t nodeId,
									storage::LogicalGraphReadDirection direction,
									LogicalGraphAdjacencyCursorOptions options);

		void open();
		[[nodiscard]] std::optional<storage::LogicalGraphAdjacencyPage> next();
		void close() noexcept;
		[[nodiscard]] bool isOpen() const noexcept { return snapshot_ != nullptr; }

	private:
		const storage::LogicalGraphReadBackend &backend_;
		uint64_t node_id_ = 0U;
		storage::LogicalGraphReadDirection direction_;
		LogicalGraphAdjacencyCursorOptions options_;
		std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot_;
		std::optional<storage::LogicalGraphAdjacencyContinuation> continuation_;
		size_t edges_returned_ = 0U;
		bool exhausted_ = false;
	};

} // namespace graph::query::execution
