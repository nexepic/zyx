#pragma once
#include "VersionedRows.hpp"
#include <array>
#include <limits>

namespace graph::storage::nativewal {
	struct AdjacencyOrder {
		using Posting = std::pair<uint64_t, uint64_t>;
		VersionCharge charge;
		VersionMemoryResource memory;
		std::pmr::vector<Posting> outgoing, incoming;
		AdjacencyOrder(const std::shared_ptr<VersionBudget> &budget, size_t count)
			: charge(budget, sizeof(AdjacencyOrder) + 64), memory(budget), outgoing(&memory), incoming(&memory) {
			outgoing.reserve(count);
			incoming.reserve(count);
		}
		void sort() {
			std::ranges::sort(outgoing);
			std::ranges::sort(incoming);
		}
		void merge(const AdjacencyOrder &a, const AdjacencyOrder &b) {
			std::ranges::merge(a.outgoing, b.outgoing, std::back_inserter(outgoing));
			std::ranges::merge(a.incoming, b.incoming, std::back_inserter(incoming));
		}
	};
	struct AdjacencyChanges {
		static constexpr size_t levelCount = std::numeric_limits<size_t>::digits;
		VersionCharge charge;
		std::array<std::shared_ptr<const AdjacencyOrder>, levelCount> levels{};
		explicit AdjacencyChanges(const std::shared_ptr<VersionBudget> &b)
			: charge(b, sizeof(AdjacencyChanges) + 64) {}
	};

	// Each range contains one owner only. Coalesce the same ID across
	// immutable runs before checking its current authoritative endpoints.
	class AdjacentCursor {
		using Range = std::span<const AdjacencyOrder::Posting>;
		std::array<Range, 1 + AdjacencyChanges::levelCount> ranges;
		size_t count = 0;
		uint64_t owner, after;
		bool outgoing;
	public:
		AdjacentCursor(uint64_t id, bool out, uint64_t previous) : owner(id), after(previous), outgoing(out) {}
		void add(const AdjacencyOrder &order) {
			const auto &v = outgoing ? order.outgoing : order.incoming;
			auto first = std::ranges::upper_bound(v, AdjacencyOrder::Posting{owner, after});
			auto last = std::upper_bound(first, v.end(), AdjacencyOrder::Posting{owner, UINT64_MAX});
			if (first != last) ranges[count++] = Range(first, last);
		}
		uint64_t next() {
			uint64_t id = UINT64_MAX;
			for (size_t i = 0; i < count; ++i) id = std::min(id, ranges[i].front().second);
			if (id == UINT64_MAX) return 0;
			for (size_t i = count; i--;) {
				auto &r = ranges[i];
				while (!r.empty() && r.front().second == id) r = r.subspan(1);
				if (r.empty()) r = ranges[--count];
			}
			return id;
		}
	};
}
