#pragma once

#include <algorithm>
#include <optional>
#include <limits>
#include <stdexcept>
#include <span>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

namespace graph::storage::detail {

// Keep only present owners, then group physical reads without changing the
// caller's logical locations. Cancellation also bounds CPU work between I/O.
inline std::vector<size_t> propertyPageOrder(
	std::span<const std::optional<EntityPropertyLocation>> locations,
	const CancellationCheckpoint *cancellation,
	const SelectedPropertyReadAdmission *bufferAdmission = nullptr) {
	const auto check = [&] { if (cancellation != nullptr) cancellation->check(); };
	check();
	size_t count = 0U;
	for (size_t index = 0U; index < locations.size(); ++index) {
		if ((index & 255U) == 0U) check();
		if (locations[index].has_value()) ++count;
	}
	std::vector<size_t> present;
	if (count > std::numeric_limits<size_t>::max() / sizeof(size_t)) throw std::overflow_error("Property page order slots overflowed");
	if (bufferAdmission) (*bufferAdmission)(count * sizeof(size_t));
	present.reserve(count);
	if (bufferAdmission) (*bufferAdmission)(present.capacity() * sizeof(size_t));
	for (size_t index = 0U; index < locations.size(); ++index) {
		if ((index & 255U) == 0U) check();
		if (locations[index].has_value()) present.push_back(index);
	}
	size_t comparisons = 0U;
	std::ranges::sort(present, [&](size_t left, size_t right) {
		if ((comparisons++ & 255U) == 0U) check();
		const auto &lhs = *locations[left];
		const auto &rhs = *locations[right];
		return lhs.page_id != rhs.page_id ? lhs.page_id < rhs.page_id : lhs.slot_id < rhs.slot_id;
	});
	check();
	return present;
}

} // namespace graph::storage::detail
