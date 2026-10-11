#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace graph::query::execution::detail {

// Sort private gather storage in place. A failed checkpoint may change its
// contents, so callers publish or probe IDs only after this function succeeds.
// Capacity is unchanged and remains charged by the owning reservation.
template<class Allocator, class Check>
void sortUniqueIdsGuarded(std::vector<uint64_t, Allocator> &ids, Check &&check) {
	check();
	std::size_t comparisons = 0U;
	std::sort(ids.begin(), ids.end(), [&](uint64_t left, uint64_t right) {
		if ((comparisons++ & 255U) == 0U) check();
		return left < right;
	});
	check();
	std::size_t write = ids.empty() ? 0U : 1U;
	for (std::size_t read = 1U; read < ids.size(); ++read) {
		if ((read & 255U) == 0U) check();
		if (ids[read] != ids[write - 1U]) ids[write++] = ids[read];
	}
	check();
	ids.resize(write);
}

} // namespace graph::query::execution::detail
