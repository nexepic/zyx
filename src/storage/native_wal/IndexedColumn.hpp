#pragma once
#include <algorithm>
#include <bit>
#include "VersionedRows.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
namespace graph::storage::nativewal {
	// Covered integers are also exact in the executor's mixed numeric comparisons.
	inline bool exactColumnInteger(int64_t v) {
		constexpr auto limit = (int64_t{1} << 53) - 1;
		return v >= -limit && v <= limit;
	}
	struct ColumnDeclaration {
		std::string label, key;
	};
	using Posting = std::pair<int64_t, uint64_t>;
	struct IndexedColumn {
		VersionCharge charge;
		std::vector<std::pair<uint64_t, int64_t>> column;
		std::vector<Posting> postings;
		bool complete = true;
		explicit IndexedColumn(const std::shared_ptr<VersionBudget> &b) : charge(b, sizeof(IndexedColumn)) {}
		void observe(const ColumnDeclaration &decl, const LogicalGraphNode &n) {
			if (std::ranges::find(n.labels, decl.label) == n.labels.end())
				return;
			for (const auto &p: n.properties) {
				if (p.key != decl.key || p.value.getType() == PropertyType::NULL_TYPE)
					continue;
				const auto *v = std::get_if<int64_t>(&p.value.getVariant());
				if (!v || !exactColumnInteger(*v)) {
					complete = false;
					return;
				}
				if (column.size() == column.capacity()) {
					auto count = std::max<size_t>(1, 2 * column.size());
					// Include old allocations during vector growth.
					charge.growTo(sizeof(IndexedColumn) +
								  (count + column.size()) * (sizeof(column[0]) + sizeof(Posting)));
					column.reserve(count);
					postings.reserve(count);
				}
				column.emplace_back(n.node_id, *v);
				postings.emplace_back(*v, n.node_id);
				return;
			}
		}
	};
	// Once the bounded list overflows, ordinary row scans serve this generation
	// until the next checkpoint. Snapshots keep their own immutable list.
	struct ColumnChanges {
		static constexpr size_t limit = 64;
		VersionCharge charge;
		std::vector<uint64_t> ids;
		explicit ColumnChanges(const std::shared_ptr<VersionBudget> &b) :
			charge(b, sizeof(ColumnChanges) + limit * sizeof(uint64_t)) {
			ids.reserve(limit);
		}
	};
} // namespace graph::storage::nativewal
