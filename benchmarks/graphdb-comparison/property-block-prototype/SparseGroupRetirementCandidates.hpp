#pragma once

#include "SparseGroupDescriptor.hpp"

namespace zyx::benchmark {

// A local reference difference is not a reachability proof. Every candidate
// still requires an authoritative graph walk before native retirement.
class SparseGroupRetirementCandidates final {
public:
	static std::vector<graph::storage::LargeValueRef> collect(
		const graph::storage::LargeValueRef &beforeRoot, std::span<const SparseGroupDescriptor::Column> beforeColumns,
		const graph::storage::LargeValueRef &afterRoot, std::span<const SparseGroupDescriptor::Column> afterColumns) {
		using graph::storage::LargeValueRef;
		std::vector<LargeValueRef> retained{afterRoot};
		for (const auto &column : afterColumns) retained.push_back(column.reference);
		std::vector<LargeValueRef> before{beforeRoot};
		for (const auto &column : beforeColumns) before.push_back(column.reference);
		const auto ordered = [](const auto &left, const auto &right) { return left.value_id.value() < right.value_id.value(); };
		const auto normalize = [&](auto &references) {
			std::ranges::sort(references, ordered);
			for (size_t index = 0U; index < references.size(); ++index) {
				if (references[index].value_id.value() == 0U)
					throw std::invalid_argument("Retirement candidate requires a valid value identity");
				if (index != 0U && references[index - 1U].value_id == references[index].value_id &&
					references[index - 1U] != references[index])
					throw std::invalid_argument("Retirement candidate has conflicting immutable references");
			}
			references.erase(std::unique(references.begin(), references.end()), references.end());
		};
		normalize(retained); normalize(before);
		std::vector<LargeValueRef> candidates;
		for (const auto &reference : before) {
			const auto found = std::lower_bound(retained.begin(), retained.end(), reference, ordered);
			if (found != retained.end() && found->value_id == reference.value_id) {
				if (*found != reference) throw std::invalid_argument("Retirement comparison has conflicting immutable references");
			} else candidates.push_back(reference);
		}
		return candidates;
	}
};

} // namespace zyx::benchmark
