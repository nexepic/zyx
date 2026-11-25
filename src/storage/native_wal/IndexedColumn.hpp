#pragma once
#include <algorithm>
#include <bit>
#include <memory_resource>
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
	// Charge the actual vector allocations, including both buffers during a
	// reallocation. Once the old allocation is freed its admission is returned,
	// even if a reader retains the finished projection for many checkpoints.
	class ProjectionMemory final : public std::pmr::memory_resource {
		std::shared_ptr<VersionBudget> budget;
		void *do_allocate(size_t bytes, size_t alignment) override {
			budget->acquire(bytes);
			try {
				return std::pmr::new_delete_resource()->allocate(bytes, alignment);
			} catch (...) {
				budget->release(bytes);
				throw;
			}
		}
		void do_deallocate(void *p, size_t bytes, size_t alignment) noexcept override {
			std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
			budget->release(bytes);
		}
		bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }

	public:
		explicit ProjectionMemory(std::shared_ptr<VersionBudget> b) : budget(std::move(b)) {}
	};
	struct IndexedColumn {
		VersionCharge charge;
		ProjectionMemory memory;
		std::pmr::vector<std::pair<uint64_t, int64_t>> column{&memory};
		std::pmr::vector<Posting> postings{&memory};
		size_t sourceRows;
		bool complete = true;
		IndexedColumn(const std::shared_ptr<VersionBudget> &b, size_t rows) :
			charge(b, sizeof(IndexedColumn)), memory(b), sourceRows(rows) {}
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
					const auto size = column.size();
					if (size >= sourceRows)
						throw std::logic_error("Projection exceeds its source row count");
					column.reserve(size + std::min(sourceRows - size, std::max<size_t>(size, 1)));
				}
				column.emplace_back(n.node_id, *v);
				return;
			}
		}
		// Partial integer coverage is useful for individual property reads,
		// but no candidate or uniqueness lookup may use its sorted postings.
		void releaseUnusablePostings() {
			if (!complete)
				std::pmr::vector<Posting>{&memory}.swap(postings);
		}
		void buildPostings() {
			postings.reserve(column.size());
			for (const auto &[id, value]: column)
				postings.emplace_back(value, id);
			std::ranges::sort(postings);
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
