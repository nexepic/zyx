#pragma once
#include "../../src/storage/native_wal/IndexedColumn.hpp"

namespace projection_test {
	inline void exactBudget() {
		using namespace graph::storage;
		using namespace nativewal;
		constexpr size_t rows = 1000;
		constexpr size_t exact =
				sizeof(IndexedColumn) + rows * (sizeof(Posting) + sizeof(std::pair<uint64_t, int64_t>));
		for (const size_t capacity: {exact, exact - 1}) {
			auto budget = std::make_shared<VersionBudget>(capacity);
			{
				IndexedColumn projected(budget, rows);
				const ColumnDeclaration declaration{"Item", "k"};
				for (uint64_t id = 0; id < rows; ++id)
					projected.observe(
							declaration,
							LogicalGraphNode{id, {"Item"}, {{"k", graph::PropertyValue(int64_t(rows - id))}}});
				bool denied = false;
				try {
					projected.buildPostings();
				} catch (const std::length_error &) {
					denied = true;
				}
				if (denied != (capacity < exact) || (!denied && (projected.postings.size() != rows ||
																 projected.postings.front() != Posting{1, rows - 1} ||
																 projected.postings.back() != Posting{rows, 0})))
					throw std::runtime_error("Incorrect projected index budget boundary or ordering");
			}
			// Including when its second buffer was denied, the destroyed projection
			// must leave enough admission for another owner to claim the full limit.
			VersionCharge recovered(budget, capacity);
		}
	}
} // namespace projection_test
