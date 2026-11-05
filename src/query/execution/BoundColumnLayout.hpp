#pragma once

#include <memory_resource>
#include <algorithm>
#include <cstring>
#include <optional>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "query/execution/BatchIdentity.hpp"
#include "query/execution/BudgetMemoryResource.hpp"
#include "query/execution/TrivialBudgetBuffer.hpp"

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
	struct NameRecord { size_t offset, length; };
public:
	BoundColumnLayout(std::span<const std::string> names, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource(),
		const std::function<void()> &checkpoint = {})
		: memory_(budget, upstream), names_(&memory_), text_(&memory_), budget_(&budget) {
		initialize(names, upstream, checkpoint);
	}
	BoundColumnLayout(std::in_place_t, std::span<const std::string_view> names, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource(),
		const std::function<void()> &checkpoint = {})
		: memory_(budget, upstream), names_(&memory_), text_(&memory_), budget_(&budget) {
		initialize(names, upstream, checkpoint);
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
			if (storedName(index) == name) return column(index);
		return std::nullopt;
	}
	[[nodiscard]] std::string_view name(BoundColumnId column) const {
		if (column.layout != identity_.row(0U).owner)
			throw std::invalid_argument("column descriptor belongs to another layout");
		return storedName(column.index);
	}
	void validate(std::span<const std::string> names) const { validateNames(names); }
	void validateViews(std::span<const std::string_view> names) const { validateNames(names); }
private:
	[[nodiscard]] std::string_view storedName(size_t index) const {
		const auto &entry = names_.at(index);
		return {text_.data() + entry.offset, entry.length};
	}
	template<class Name>
	void validateNames(std::span<const Name> names) const {
		if (names.size() != names_.size()) throw std::logic_error("bound column layout changed its width");
		for (size_t index = 0U; index < names.size(); ++index)
			if (std::string_view(names[index]) != storedName(index))
				throw std::logic_error("bound column layout changed its names or order");
	}

	template<class Name>
	void initialize(std::span<const Name> names, std::pmr::memory_resource *upstream, const std::function<void()> &checkpoint) {
		if (!upstream || names.size() > (std::numeric_limits<uint32_t>::max)())
			throw std::invalid_argument("column layout requires a valid allocator and uint32 column count");
		const auto guard = [&] { if (checkpoint) checkpoint(); };
		size_t bytes = 0U;
		for (const auto &name : names) {
			guard();
			if (name.empty()) throw std::invalid_argument("column layout has an empty name");
			if (name.size() > (std::numeric_limits<size_t>::max)() - bytes)
				throw std::length_error("column layout name bytes overflowed");
			bytes += name.size();
		}
		guard();
		// Validate in O(n log n) with one admitted flat array rather than tree
		// nodes/proxies. Its input borrows expire before owning storage is built.
		TrivialBudgetBuffer<std::string_view> unique(&memory_);
		unique.reserve(names.size());
		for (const auto &name : names) { guard(); unique.push_back(std::string_view(name)); }
		if (unique.size() > 1U) std::sort(unique.begin(), unique.end());
		for (size_t index = 1U; index < unique.size(); ++index) {
			guard();
			if (unique[index - 1U] == unique[index]) throw std::invalid_argument("column layout has duplicate names");
		}
		unique.release();
		names_.reserve(names.size()); text_.resize(bytes);
		size_t offset = 0U;
		for (const auto &name : names) {
			guard();
			std::memcpy(text_.data() + offset, name.data(), name.size());
			names_.push_back(NameRecord{offset, name.size()}); offset += name.size();
		}
		guard(); // Temporary validation nodes refund before publication.
	}

	BatchIdentity identity_;
	BudgetMemoryResource memory_;
	TrivialBudgetBuffer<NameRecord> names_;
	TrivialBudgetBuffer<char> text_;
	const MaterializedByteBudget *budget_;
};

} // namespace graph::query::execution::detail
