#pragma once

#include <algorithm>
#include <limits>
#include <span>

#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "query/execution/BoundSlotRegistry.hpp"

namespace graph::query::execution::detail {

enum class ProjectionReferenceKind { PRK_DYNAMIC, PRK_VALUE, PRK_PROPERTY };

struct BoundProjectionReference {
	ProjectionReferenceKind kind = ProjectionReferenceKind::PRK_DYNAMIC;
	std::optional<BoundSlotId> source;
	const expressions::VariableReferenceExpression *expression = nullptr;
};

// Transitional input adapter: compile names once, then resolve the old row's
// map at its boundary. Instructions consume slots, never retained map iterators.
// This query-local owner borrows immutable expressions only; it cannot enter a
// plan cache or keep entity/property tokens beyond the caller's snapshot.
class BoundProjectionReferences final {
public:
	BoundProjectionReferences(std::span<const std::string> inputs,
		std::span<const logical::LogicalProjectItem> items, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: registry_(std::max<size_t>(1U, inputs.size()), 1U, nameBytes(inputs), budget, upstream),
		  memory_(budget, upstream), instructions_(&memory_), values_(&memory_) {
		for (const auto &name : inputs) {
			if (!name.empty() && !registry_.resolve(registry_.root(), name))
				(void) registry_.declare(registry_.root(), name);
		}
		instructions_.reserve(items.size());
		for (const auto &item : items) {
			BoundProjectionReference instruction;
			const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(item.expression.get());
			if (reference != nullptr) {
				instruction.expression = reference;
				// WITH n.value exposes a column named "n.value". That column wins
				// over property interpretation, exactly as in the semantic evaluator.
				if (reference->hasProperty()) {
					// Match the canonical text without an uncharged temporary string.
					const std::string_view variable = reference->getVariableName();
					for (const auto &input : inputs) {
						const std::string_view text = input;
						if (text.size() > variable.size() && text.starts_with(variable) &&
							text[variable.size()] == '.' && text.substr(variable.size() + 1U) == reference->getPropertyName()) {
							instruction.source = registry_.resolve(registry_.root(), text);
							break;
						}
					}
					if (instruction.source) instruction.kind = ProjectionReferenceKind::PRK_VALUE;
				}
				if (!instruction.source) {
					instruction.source = registry_.resolve(registry_.root(), reference->getVariableName());
					if (instruction.source)
						instruction.kind = reference->hasProperty() ? ProjectionReferenceKind::PRK_PROPERTY
							: ProjectionReferenceKind::PRK_VALUE;
				}
			}
			instructions_.push_back(instruction);
		}
		values_.resize(registry_.slotCount(), nullptr);
		registry_.seal();
	}
	BoundProjectionReferences(const BoundProjectionReferences &) = delete;
	BoundProjectionReferences &operator=(const BoundProjectionReferences &) = delete;
	BoundProjectionReferences(BoundProjectionReferences &&) = delete;
	BoundProjectionReferences &operator=(BoundProjectionReferences &&) = delete;

	class RowBinding final {
	public:
		RowBinding(BoundProjectionReferences &program, const LogicalPlanReadRow &row) : program_(program) {
			program_.bindRow(row);
		}
		~RowBinding() { program_.clearRow(); }
		RowBinding(const RowBinding &) = delete;
		RowBinding &operator=(const RowBinding &) = delete;
	private:
		BoundProjectionReferences &program_;
	};
	[[nodiscard]] const BoundProjectionReference &instruction(size_t index) const { return instructions_.at(index); }
	[[nodiscard]] const LogicalPlanReadValue *value(const BoundProjectionReference &instruction) const {
		if (!row_bound_) throw std::logic_error("projection input slots have no active row");
		if (!instruction.source) return nullptr;
		(void) registry_.name(*instruction.source); // Reject descriptors from another program.
		return values_.at(instruction.source->index);
	}
	[[nodiscard]] size_t slotCount() const noexcept { return registry_.slotCount(); }

private:
	static size_t nameBytes(std::span<const std::string> inputs) {
		size_t bytes = 1U;
		for (const auto &name : inputs) {
			if (name.size() > (std::numeric_limits<size_t>::max)() - bytes)
				throw std::overflow_error("projection binding names exceed the size limit");
			bytes += name.size();
		}
		return bytes;
	}
	void bindRow(const LogicalPlanReadRow &row) {
		if (row_bound_) throw std::logic_error("projection input slots are already bound");
		for (const auto &[name, value] : row.values) {
			if (const auto slot = registry_.resolve(registry_.root(), name)) values_[slot->index] = &value;
		}
		row_bound_ = true;
	}
	void clearRow() noexcept {
		std::fill(values_.begin(), values_.end(), nullptr);
		row_bound_ = false;
	}
	BoundSlotRegistry registry_;
	BudgetMemoryResource memory_;
	std::pmr::vector<BoundProjectionReference> instructions_;
	std::pmr::vector<const LogicalPlanReadValue *> values_;
	bool row_bound_ = false;
};

} // namespace graph::query::execution::detail
