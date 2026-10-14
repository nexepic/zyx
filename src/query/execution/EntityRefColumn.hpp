#pragma once
#include "graph/storage/LogicalGraphRead.hpp"
#include "query/execution/FixedWidthColumn.hpp"
#include "query/execution/BatchIdentity.hpp"

namespace graph::query::execution::detail {
class EntityRefColumn final {
public:
	EntityRefColumn(size_t maximumRows, const storage::LogicalGraphReadSnapshot &snapshot, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: maximum_rows_(maximumRows), budget_(&budget), snapshot_(&snapshot), fence_(snapshot.mutationFence()), schema_(snapshot.bindingSchemaIdentity()),
		  memory_(budget, upstream), values_(&memory_), states_(&memory_) {
		if (!upstream || maximumRows == 0U || maximumRows > UINT32_MAX)
			throw std::invalid_argument("entity reference column requires bounded rows and allocator");
	}
	EntityRefColumn(const EntityRefColumn &) = delete;
	EntityRefColumn &operator=(const EntityRefColumn &) = delete;
	[[nodiscard]] size_t size() const noexcept { return published_; }
	[[nodiscard]] const MaterializedByteBudget *budgetDomain() const noexcept { return budget_; }
	[[nodiscard]] const storage::LogicalGraphReadSnapshot *snapshotDomain() const noexcept { return snapshot_; }
	[[nodiscard]] BatchRowId borrowVersion() const { return identity_.row(0U); }
	void validateBorrowVersion(BatchRowId version) const {
		identity_.validate(version);
		if (snapshot_->mutationFence() != fence_ || snapshot_->bindingSchemaIdentity() != schema_)
			throw std::invalid_argument("entity column data snapshot changed");
	}
	[[nodiscard]] ColumnCellState state(BatchRowId row) const { validate(row); return states_.at(row.index); }
	[[nodiscard]] storage::EntityRef value(BatchRowId row) const {
		if (state(row) != ColumnCellState::CCS_VALUE) throw std::logic_error("entity cell has no value");
		return values_.at(row.index);
	}
	template<class Guard>
	void append(std::optional<storage::EntityRef> value, ColumnCellState state, Guard guard) {
		identity_.advance(); guard();
		if (published_ == maximum_rows_) throw std::length_error("entity column row bound exceeded");
		if (state != ColumnCellState::CCS_VALUE && state != ColumnCellState::CCS_NULL && state != ColumnCellState::CCS_MISSING)
			throw std::invalid_argument("invalid entity cell state");
		if ((state == ColumnCellState::CCS_VALUE) != value.has_value()) throw std::invalid_argument("entity value disagrees with cell state");
		if (value && (value->entity_id == 0U || (!value->is_edge && (value->source_node_id != 0U || value->target_node_id != 0U || value->type_token != 0U)) ||
			(value->is_edge && ((value->source_node_id == 0U) != (value->target_node_id == 0U)))))
			throw std::invalid_argument("entity reference has invalid identity/topology");
		const auto previous = published_;
		try {
			guard(); values_.reserve(previous + 1U); guard(); states_.reserve(previous + 1U);
			values_.push_back(value.value_or(storage::EntityRef{})); states_.push_back(state);
			guard(); published_ = previous + 1U;
		} catch (...) { values_.resize(previous); states_.resize(previous); throw; }
	}
	void clear() noexcept { identity_.invalidate(); values_.clear(); states_.clear(); published_ = 0U; }
	void release() noexcept {
		clear(); std::pmr::vector<storage::EntityRef>(&memory_).swap(values_);
		std::pmr::vector<ColumnCellState>(&memory_).swap(states_);
	}
private:
	void validate(BatchRowId row) const { validateBorrowVersion(row); if (row.index >= published_) throw std::out_of_range("entity row exceeds its published prefix"); }
	size_t maximum_rows_, published_ = 0U;
	const MaterializedByteBudget *budget_;
	const storage::LogicalGraphReadSnapshot *snapshot_;
	std::optional<storage::LogicalGraphMutationFence> fence_;
	std::optional<storage::LogicalGraphSchemaIdentity> schema_;
	BatchIdentity identity_;
	BudgetMemoryResource memory_;
	std::pmr::vector<storage::EntityRef> values_;
	std::pmr::vector<ColumnCellState> states_;
};
}
