#pragma once

#include <memory_resource>
#include <optional>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "query/execution/BatchIdentity.hpp"
#include "query/execution/BudgetMemoryResource.hpp"

namespace graph::query::execution::detail {

struct BoundColumnId {
	uint32_t index;
	uint64_t layout;
	bool operator==(const BoundColumnId &) const = default;
};

// Explicit legacy adapter for callers that cannot admit an owning layout.
inline void validateColumnBinding(std::span<const std::string> names, size_t index, std::string_view expected) {
	if (index >= names.size() || names[index] != expected)
		throw std::logic_error("bound input column changed its name or position");
}

// Immutable query-local name layout, independent of the actual value types of
// a batch. Dynamic properties must still be guarded by each typed consumer.
class BoundColumnLayout final {
public:
	BoundColumnLayout(std::span<const std::string> names, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource(),
		const std::function<void()> &checkpoint = {})
		: memory_(budget, upstream), names_(&memory_), budget_(&budget) {
		if (!upstream || names.size() > (std::numeric_limits<uint32_t>::max)())
			throw std::invalid_argument("column layout requires a valid allocator and uint32 column count");
		const auto guard = [&] { if (checkpoint) checkpoint(); };
		for (const auto &name : names) { guard(); if (name.empty()) throw std::invalid_argument("column layout has an empty name"); }
		guard();
		names_.reserve(names.size());
		std::pmr::set<std::string_view> unique(&memory_);
		for (const auto &name : names) {
			guard();
			if (!unique.insert(name).second) throw std::invalid_argument("column layout has duplicate names");
			names_.emplace_back(name);
		}
		guard(); // Temporary validation nodes refund before publication.
	}
	BoundColumnLayout(const BoundColumnLayout &) = delete;
	BoundColumnLayout &operator=(const BoundColumnLayout &) = delete;
	BoundColumnLayout(BoundColumnLayout &&) = delete;
	BoundColumnLayout &operator=(BoundColumnLayout &&) = delete;
	[[nodiscard]] size_t size() const noexcept { return names_.size(); }
	[[nodiscard]] const MaterializedByteBudget *budgetDomain() const noexcept { return budget_; }
	[[nodiscard]] BoundColumnId column(size_t index) const {
		(void) names_.at(index);
		return {static_cast<uint32_t>(index), identity_.row(0U).owner};
	}
	[[nodiscard]] std::optional<BoundColumnId> find(std::string_view name) const {
		for (size_t index = 0U; index < names_.size(); ++index)
			if (names_[index] == name) return column(index);
		return std::nullopt;
	}
	[[nodiscard]] std::string_view name(BoundColumnId column) const {
		if (column.layout != identity_.row(0U).owner)
			throw std::invalid_argument("column descriptor belongs to another layout");
		return names_.at(column.index);
	}
	void validate(std::span<const std::string> names) const {
		if (names.size() != names_.size()) throw std::logic_error("bound column layout changed its width");
		for (size_t index = 0U; index < names.size(); ++index)
			if (std::string_view(names[index]) != std::string_view(names_[index]))
				throw std::logic_error("bound column layout changed its names or order");
	}
private:
	BatchIdentity identity_;
	BudgetMemoryResource memory_;
	std::pmr::vector<std::pmr::string> names_;
	const MaterializedByteBudget *budget_;
};

} // namespace graph::query::execution::detail
