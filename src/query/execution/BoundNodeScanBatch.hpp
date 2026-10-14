#pragma once

#include "query/execution/BatchEnvelope.hpp"
#include "query/execution/BoundExecutionProgram.hpp"
#include "graph/query/execution/SelectionVector.hpp"

namespace graph::query::execution::detail {

// Query-local publication boundary for native selected-property scans. Payload
// matrices stay owned/charged by the source until its final row handoff. This
// object must be released before those matrices are moved or modified.
class BoundNodeScanBatch final {
public:
	template<class Guard>
	BoundNodeScanBatch(const BoundExecutionProgram &program, const storage::LogicalGraphReadSnapshot &snapshot,
		std::span<const std::string> keys, std::span<const std::vector<PropertyValue>> columns,
		std::span<const uint64_t> nodeIds, MaterializedByteBudget &budget, Guard guard)
		: program_(program), snapshot_(snapshot), memory_(budget), entity_name_(&memory_), names_(&memory_),
		  sources_(&memory_), selected_(&memory_), ordinals_(&memory_), numeric_(budget), entities_(std::max<size_t>(nodeIds.size(), 1U), snapshot, budget) {
		program.validateSnapshot(snapshot);
		if (keys.size() != columns.size()) throw std::invalid_argument("scan batch changed its column width");
		for (const auto &column : columns) if (column.size() != nodeIds.size())
			throw std::invalid_argument("scan batch changed its row count");
		entity_name_ = "$entity";
		while (std::ranges::any_of(keys, [&](const auto &key) { return std::string_view(key) == std::string_view(entity_name_); })) {
			guard(); entity_name_.push_back('$');
		}
		for (const auto &key : keys) { guard(); names_.push_back(key); }
		names_.push_back(entity_name_);
		layout_.emplace(std::in_place, std::span<const std::string_view>(names_), budget, std::pmr::get_default_resource(), guard);
		std::pmr::vector<size_t> requested(&memory_);
		for (size_t index = 0U; index < keys.size(); ++index) { guard(); requested.push_back(index); }
		if (!numeric_.tryPrepare(columns, requested, guard)) throw MaterializedByteLimitExceeded();
		for (size_t index = 0U; index < keys.size(); ++index) { guard(); sources_.push_back(BatchColumnSource::numeric(numeric_, index)); }
		uint64_t previous = 0U;
		for (uint64_t id : nodeIds) {
			guard(); if (id == 0U || id <= previous) throw std::invalid_argument("scan batch requires ordered unique Node IDs");
			entities_.append(storage::EntityRef{id, false, 0U, 0U, 0U}, ColumnCellState::CCS_VALUE, guard); previous = id;
		}
		sources_.push_back(BatchColumnSource::entity(entities_));
		for (size_t row = 0U; row < nodeIds.size(); ++row) { guard(); selected_.push_back(static_cast<uint32_t>(row)); ordinals_.push_back(row); }
		envelope_.emplace(program.root().program, *layout_, snapshot, std::max<size_t>(nodeIds.size(), 1U), budget);
		envelope_->publish(std::span<const std::string_view>(names_), sources_, nodeIds.size(), selected_, ordinals_, guard);
	}
	BoundNodeScanBatch(const BoundNodeScanBatch &) = delete;
	BoundNodeScanBatch &operator=(const BoundNodeScanBatch &) = delete;
	[[nodiscard]] const BatchEnvelope &envelope() const { program_.validateSnapshot(snapshot_); return *envelope_; }
	[[nodiscard]] const BoundColumnLayout &layout() const { return *layout_; }
	[[nodiscard]] storage::EntityRef entity(size_t row) const {
		return std::get<storage::EntityRef>(envelope().cell(layout_->column(layout_->size() - 1U), envelope_->rowId(row)).data);
	}
	template<class Guard>
	void publishSelection(const SelectionVector &selection, std::span<const size_t> originalOrdinals, Guard guard) {
		if (selection.size() != ordinals_.size()) throw std::invalid_argument("scan selection changed row count");
		selected_.clear();
		for (size_t row : originalOrdinals) {
			guard(); if (row >= selection.size()) throw std::out_of_range("scan original ordinal exceeds batch");
			if (selection.includes(row)) selected_.push_back(static_cast<uint32_t>(row));
		}
		envelope_->publish(std::span<const std::string_view>(names_), sources_, selection.size(), selected_, ordinals_, guard);
	}
private:
	const BoundExecutionProgram &program_;
	const storage::LogicalGraphReadSnapshot &snapshot_;
	BudgetMemoryResource memory_;
	std::pmr::string entity_name_;
	std::pmr::vector<std::string_view> names_;
	std::pmr::vector<BatchColumnSource> sources_;
	std::pmr::vector<uint32_t> selected_;
	std::pmr::vector<uint64_t> ordinals_;
	PrimitiveNumericColumns numeric_;
	EntityRefColumn entities_;
	std::optional<BoundColumnLayout> layout_;
	std::optional<BatchEnvelope> envelope_;
};
}
