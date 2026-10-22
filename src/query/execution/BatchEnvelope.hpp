#pragma once

#include <variant>
#include "query/execution/BoundColumnLayout.hpp"
#include "query/execution/FixedWidthBatch.hpp"
#include "query/execution/PrimitiveNumericColumns.hpp"
#include "query/execution/EntityRefColumn.hpp"
#include "query/execution/DynamicValueArena.hpp"
#include "query/execution/DirectPropertyColumns.hpp"

namespace graph::query::execution::detail {
enum class BatchColumnKind { BCK_INTEGER, BCK_DOUBLE, BCK_BOOLEAN, BCK_ENTITY, BCK_DYNAMIC, BCK_LEGACY_VALUE, BCK_DIRECT_VALUE };
struct DynamicCellView { const DynamicValueArena *arena; DynamicValueId value; };
struct BatchCellView {
	ColumnCellState state = ColumnCellState::CCS_NULL;
	std::variant<std::monostate, int64_t, double, bool, storage::EntityRef, DynamicCellView, const PropertyValue *, std::string_view> data;
};

// Internal borrowed payload descriptor. Metadata is copied into the envelope;
// payload owners remain task-local and must outlive every envelope borrow.
struct BatchColumnSource {
	const void *owner;
	const MaterializedByteBudget *budget;
	BatchRowId version;
	size_t rows, ordinal;
	BatchColumnKind kind;
	bool exact_presence;
	void (*validate)(const void *, BatchRowId);
	BatchCellView (*read)(const void *, BatchRowId, size_t);
	const storage::LogicalGraphReadSnapshot *entity_snapshot = nullptr;

	static BatchColumnSource direct(const DirectPropertyColumns &source, size_t ordinal) {
		return {&source, source.budgetDomain(), source.borrowVersion(), source.size(), ordinal, BatchColumnKind::BCK_DIRECT_VALUE, true,
			[](const void *owner, BatchRowId version) { static_cast<const DirectPropertyColumns *>(owner)->validateBorrowVersion(version); },
			[](const void *owner, BatchRowId row, size_t column) {
				const auto cell = static_cast<const DirectPropertyColumns *>(owner)->cell(column, row);
				BatchCellView result; result.state = cell.state;
				std::visit([&](auto value) { result.data = value; }, cell.data);
				return result;
			}};
	}

	template<size_t Index, class... Ts>
	static BatchColumnSource fixed(const FixedWidthBatch<Ts...> &source) {
		using T = std::tuple_element_t<Index, std::tuple<Ts...>>;
		constexpr auto kind = std::is_same_v<T, int64_t> ? BatchColumnKind::BCK_INTEGER :
			std::is_same_v<T, double> ? BatchColumnKind::BCK_DOUBLE : BatchColumnKind::BCK_BOOLEAN;
		return {&source, source.budgetDomain(), source.borrowVersion(), source.size(), Index, kind, true,
			[](const void *owner, BatchRowId version) { static_cast<const FixedWidthBatch<Ts...> *>(owner)->validateBorrowVersion(version); },
			[](const void *owner, BatchRowId row, size_t) {
				const auto cell = static_cast<const FixedWidthBatch<Ts...> *>(owner)->template cell<Index>(row);
				BatchCellView result; result.state = cell.state(); if (result.state == ColumnCellState::CCS_VALUE) result.data = cell.value(); return result;
			}};
	}
	static BatchColumnSource numeric(const PrimitiveNumericColumns &source, size_t ordinal) {
		const auto kind = source.kind(ordinal);
		return {&source, source.budgetDomain(), source.borrowVersion(), source.size(), ordinal,
			kind == NumericColumnKind::NCK_INTEGER ? BatchColumnKind::BCK_INTEGER : kind == NumericColumnKind::NCK_DOUBLE ? BatchColumnKind::BCK_DOUBLE : BatchColumnKind::BCK_LEGACY_VALUE,
			false, // Name-column bridge erases missing vs stored NULL; presence consumers must use owner enumeration.
			[](const void *owner, BatchRowId version) { static_cast<const PrimitiveNumericColumns *>(owner)->validateBorrowVersion(version); },
			[](const void *owner, BatchRowId row, size_t column) {
				const auto cell = static_cast<const PrimitiveNumericColumns *>(owner)->cell(column, row); BatchCellView result;
				std::visit([&](auto value) { using T = decltype(value); if constexpr (!std::is_same_v<T, std::monostate>) { result.state = ColumnCellState::CCS_VALUE; result.data = value; } }, cell);
				return result;
			}};
	}
	static BatchColumnSource entity(const EntityRefColumn &source) {
		return {&source, source.budgetDomain(), source.borrowVersion(), source.size(), 0U, BatchColumnKind::BCK_ENTITY, true,
			[](const void *owner, BatchRowId version) { static_cast<const EntityRefColumn *>(owner)->validateBorrowVersion(version); },
			[](const void *owner, BatchRowId row, size_t) {
				const auto &source = *static_cast<const EntityRefColumn *>(owner); BatchCellView result; result.state = source.state(row);
				if (result.state == ColumnCellState::CCS_VALUE) result.data = source.value(row); return result;
			}, source.snapshotDomain()};
	}
	static BatchColumnSource dynamic(const DynamicValueArena &source, DynamicForestId forest) {
		const BatchRowId version{0U, forest.owner, forest.incarnation}; source.validateBorrowVersion(version);
		if (forest.first > source.rootCount() || forest.count > source.rootCount() - forest.first)
			throw std::out_of_range("dynamic column forest exceeds its roots");
		return {&source, source.budgetDomain(), version, static_cast<size_t>(forest.count), static_cast<size_t>(forest.first), BatchColumnKind::BCK_DYNAMIC, true,
			[](const void *owner, BatchRowId version) { static_cast<const DynamicValueArena *>(owner)->validateBorrowVersion(version); },
			[](const void *owner, BatchRowId row, size_t first) {
				const auto &source = *static_cast<const DynamicValueArena *>(owner); source.validateBorrowVersion(row);
				const auto id = source.rootId(first + row.index); BatchCellView result; result.state = source.state(id);
				if (result.state == ColumnCellState::CCS_VALUE) result.data = DynamicCellView{&source, id}; return result;
			}};
	}
};

// Complete flat batch metadata: immutable layout/program, data snapshot, typed
// or declared legacy columns, selection/order and independently versioned rows.
// Query snapshot/layout/budget and all payload owners outlive this task object.
class BatchEnvelope final {
public:
	BatchEnvelope(uint64_t program, const BoundColumnLayout &layout, const storage::LogicalGraphReadSnapshot &snapshot,
		size_t maximumRows, MaterializedByteBudget &budget, std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: program_(program), layout_(layout), snapshot_(snapshot), fence_(snapshot.mutationFence()), schema_(snapshot.bindingSchemaIdentity()),
		  token_support_(snapshot.supportsPropertyTokenResolution()), immutable_identity_(snapshot.hasImmutableReadIdentity()),
		  maximum_rows_(maximumRows), budget_(&budget), memory_(budget, upstream), columns_(&memory_), selected_(&memory_), ordinals_(&memory_) {
		if (!upstream || program == 0U || maximumRows == 0U || maximumRows > UINT32_MAX)
			throw std::invalid_argument("batch envelope requires program, allocator and uint32 rows");
		if (layout.budgetDomain() != &budget) throw std::invalid_argument("batch layout belongs to another budget domain");
	}
	BatchEnvelope(const BatchEnvelope &) = delete;
	BatchEnvelope &operator=(const BatchEnvelope &) = delete;
	template<class Guard>
	void publish(std::span<const std::string> names, std::span<const BatchColumnSource> sources, size_t rows,
		std::span<const uint32_t> selected, std::span<const uint64_t> originalOrdinals, Guard guard) {
		publishImpl(names, sources, rows, selected, originalOrdinals, std::move(guard));
	}
	template<class Guard>
	void publish(std::span<const std::string_view> names, std::span<const BatchColumnSource> sources, size_t rows,
		std::span<const uint32_t> selected, std::span<const uint64_t> originalOrdinals, Guard guard) {
		publishImpl(names, sources, rows, selected, originalOrdinals, std::move(guard));
	}

	[[nodiscard]] size_t size() const noexcept { return rows_; }
	[[nodiscard]] size_t selectedCount() const noexcept { return selected_.size(); }
	[[nodiscard]] BatchRowId rowId(size_t row) const {
		if (!published_ || row >= rows_) throw std::out_of_range("batch row is outside publication");
		return identity_.row(static_cast<uint32_t>(row));
	}
	[[nodiscard]] BatchRowId selectedRow(size_t index) const { return rowId(selected_.at(index)); }
	[[nodiscard]] uint64_t originalOrdinal(BatchRowId row) const { validateRow(row); return ordinals_.at(row.index); }
	[[nodiscard]] BatchCellView cell(BoundColumnId column, BatchRowId row) const {
		validateRow(row); (void) layout_.name(column); const auto &source = columns_.at(column.index);
		source.validate(source.owner, source.version);
		return source.read(source.owner, {row.index, source.version.owner, source.version.incarnation}, source.ordinal);
	}
	[[nodiscard]] BatchRowId sourceRow(BoundColumnId column, BatchRowId row) const {
		validateRow(row); (void) layout_.name(column); const auto &source = columns_.at(column.index);
		source.validate(source.owner, source.version); return {row.index, source.version.owner, source.version.incarnation};
	}
	[[nodiscard]] BatchColumnKind columnKind(BoundColumnId column) const {
		if (!published_) throw std::logic_error("batch has no published columns");
		(void) layout_.name(column); return columns_.at(column.index).kind;
	}
	[[nodiscard]] bool exactPresence(BoundColumnId column) const { (void) layout_.name(column); return columns_.at(column.index).exact_presence; }
	void validateSnapshot(const storage::LogicalGraphReadSnapshot &snapshot) const {
		if (&snapshot != &snapshot_ || snapshot.mutationFence() != fence_ || snapshot.bindingSchemaIdentity() != schema_ || snapshot.supportsPropertyTokenResolution() != token_support_)
			throw std::invalid_argument("batch belongs to another data snapshot");
	}
	[[nodiscard]] uint64_t program() const noexcept { return program_; }
	void validateProgram(uint64_t program) const { if (program != program_) throw std::invalid_argument("batch belongs to another execution program"); }
	void release() noexcept {
		identity_.invalidate(); published_ = false; rows_ = 0U;
		std::pmr::vector<BatchColumnSource>(&memory_).swap(columns_);
		std::pmr::vector<uint32_t>(&memory_).swap(selected_); std::pmr::vector<uint64_t>(&memory_).swap(ordinals_);
	}
private:
	template<class Guard, class Name>
	void publishImpl(std::span<const Name> names, std::span<const BatchColumnSource> sources, size_t rows,
		std::span<const uint32_t> selected, std::span<const uint64_t> originalOrdinals, Guard guard) {
		identity_.advance(); guard(); validateSnapshot(snapshot_);
		if constexpr (std::is_same_v<Name, std::string_view>) layout_.validateViews(names);
		else layout_.validate(names);
		if (sources.size() != layout_.size() || rows > maximum_rows_ || selected.size() > maximum_rows_ || originalOrdinals.size() != rows)
			throw std::invalid_argument("batch envelope has inconsistent rectangular metadata");
		for (const auto &source : sources) {
			guard();
			if (source.kind != BatchColumnKind::BCK_INTEGER && source.kind != BatchColumnKind::BCK_DOUBLE && source.kind != BatchColumnKind::BCK_BOOLEAN &&
				source.kind != BatchColumnKind::BCK_ENTITY && source.kind != BatchColumnKind::BCK_DYNAMIC && source.kind != BatchColumnKind::BCK_LEGACY_VALUE && source.kind != BatchColumnKind::BCK_DIRECT_VALUE)
				throw std::invalid_argument("batch column has an invalid kind");
			if (!source.owner || !source.validate || !source.read || source.budget != budget_ || source.rows != rows)
				throw std::invalid_argument("batch column has incompatible owner, budget or row count");
			if (source.kind == BatchColumnKind::BCK_ENTITY && source.entity_snapshot != &snapshot_)
				throw std::invalid_argument("entity column belongs to another data snapshot");
			source.validate(source.owner, source.version);
		}
		for (uint32_t row : selected) { guard(); if (row >= rows) throw std::out_of_range("batch selection exceeds rows"); }
		std::pmr::vector<BatchColumnSource> nextColumns(&memory_);
		std::pmr::vector<uint32_t> nextSelected(&memory_);
		std::pmr::vector<uint64_t> nextOrdinals(&memory_);
		guard(); nextColumns.assign(sources.begin(), sources.end()); guard(); nextSelected.assign(selected.begin(), selected.end());
		guard(); nextOrdinals.assign(originalOrdinals.begin(), originalOrdinals.end());
		// Every column is validated before publication or any consumer effects.
		for (const auto &source : nextColumns) for (size_t row = 0U; row < rows; ++row) {
			guard(); source.validate(source.owner, source.version);
			validateCell(source.kind, source.read(source.owner, {static_cast<uint32_t>(row), source.version.owner, source.version.incarnation}, source.ordinal));
		}
		guard(); validateSnapshot(snapshot_);
		columns_.swap(nextColumns); selected_.swap(nextSelected); ordinals_.swap(nextOrdinals); rows_ = rows; published_ = true;
	}
	static void validateCell(BatchColumnKind kind, const BatchCellView &cell) {
		if (cell.state != ColumnCellState::CCS_VALUE) {
			if ((cell.state != ColumnCellState::CCS_NULL && cell.state != ColumnCellState::CCS_MISSING) || !std::holds_alternative<std::monostate>(cell.data))
				throw std::invalid_argument("batch cell has invalid absence state/payload");
			return;
		}
		const bool valid = kind == BatchColumnKind::BCK_INTEGER ? std::holds_alternative<int64_t>(cell.data) :
			kind == BatchColumnKind::BCK_DOUBLE ? std::holds_alternative<double>(cell.data) :
			kind == BatchColumnKind::BCK_BOOLEAN ? std::holds_alternative<bool>(cell.data) :
			kind == BatchColumnKind::BCK_ENTITY ? std::holds_alternative<storage::EntityRef>(cell.data) :
			kind == BatchColumnKind::BCK_DYNAMIC ? std::holds_alternative<DynamicCellView>(cell.data) :
			kind == BatchColumnKind::BCK_DIRECT_VALUE ?
				(std::holds_alternative<int64_t>(cell.data) || std::holds_alternative<double>(cell.data) ||
				 std::holds_alternative<bool>(cell.data) || std::holds_alternative<std::string_view>(cell.data) ||
				 std::holds_alternative<const PropertyValue *>(cell.data)) :
			kind == BatchColumnKind::BCK_LEGACY_VALUE && std::holds_alternative<const PropertyValue *>(cell.data);
		if (!valid) throw std::invalid_argument("batch column kind disagrees with a cell payload");
		if (const auto *dynamic = std::get_if<DynamicCellView>(&cell.data); dynamic && (!dynamic->arena || dynamic->arena->state(dynamic->value) != ColumnCellState::CCS_VALUE))
			throw std::invalid_argument("batch dynamic cell has an invalid arena owner/state");
		if (const auto *legacy = std::get_if<const PropertyValue *>(&cell.data); legacy && !*legacy)
			throw std::invalid_argument("batch legacy cell has no owning source value");
	}
	void validateRow(BatchRowId row) const {
		identity_.validate(row); if (!published_ || row.index >= rows_) throw std::out_of_range("batch row is outside publication");
		if (!immutable_identity_) validateSnapshot(snapshot_);
	}
	uint64_t program_;
	const BoundColumnLayout &layout_;
	const storage::LogicalGraphReadSnapshot &snapshot_;
	std::optional<storage::LogicalGraphMutationFence> fence_;
	std::optional<storage::LogicalGraphSchemaIdentity> schema_;
	bool token_support_;
	bool immutable_identity_;
	size_t maximum_rows_, rows_ = 0U;
	bool published_ = false;
	const MaterializedByteBudget *budget_;
	BatchIdentity identity_;
	BudgetMemoryResource memory_;
	std::pmr::vector<BatchColumnSource> columns_;
	std::pmr::vector<uint32_t> selected_;
	std::pmr::vector<uint64_t> ordinals_;
};
}
