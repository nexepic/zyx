#pragma once

#include "query/execution/BoundExecutionProgram.hpp"
#include "query/execution/BoundPropertyBindings.hpp"
#include "query/execution/BatchIdentity.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"

namespace graph::query::execution::detail {

enum class BoundInputKind { BIK_DEFERRED, BIK_VALUE, BIK_PROPERTY, BIK_PARAMETER };
struct BoundExpressionInput {
	BoundInputKind kind = BoundInputKind::BIK_DEFERRED;
	std::optional<BoundSlotId> source;
	std::optional<BoundPropertyId> property;
	uint32_t input_ordinal = 0U;
	uint64_t parameter_owner = 0U;
};

// Sealed expression-to-slot descriptors for one operator's input boundary.
// Scoped/pattern evaluators remain declared row adapters until S2.3; traversal
// never binds a locally shadowed expression to an outer slot.
class BoundExpressionInputs final {
public:
	template<class Guard>
	BoundExpressionInputs(const BoundExecutionProgram &program, BoundOperatorId op,
		std::span<const expressions::Expression *const> expressions,
		storage::LogicalGraphReadSnapshot &snapshot, MaterializedByteBudget &budget, Guard guard,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource(), const QueryContext *context = nullptr)
		: program_(program), op_(op), context_(context), memory_(budget, upstream), instructions_(&memory_), values_(&memory_), input_ordinals_(&memory_), parameters_(&memory_) {
		if (!upstream) throw std::invalid_argument("bound expression inputs require an allocator");
		program.validateSnapshot(snapshot);
		std::pmr::vector<BoundPropertyRequest> requests(&memory_);
		std::pmr::vector<const expressions::Expression *> properties(&memory_);
		for (const auto *expression : expressions) collect(expression, requests, properties, 1U, guard);
		values_.resize(program.view(op).inputs.size(), nullptr);
		const auto inputs = program.view(op).inputs;
		for (size_t index = 0U; index < inputs.size(); ++index) {
			guard(); input_ordinals_.emplace(inputs[index].index, static_cast<uint32_t>(index));
		}
		if (!requests.empty() && snapshot.bindingSchemaIdentity()) {
			properties_.emplace(program.slots(), snapshot, requests, 65536U, 1024U * 1024U, budget, upstream, guard);
			for (size_t index = 0U; index < properties.size(); ++index)
				instructions_.at(properties[index]).property = properties_->descriptor(index);
		}
		guard(); program.validateSnapshot(snapshot);
	}
	BoundExpressionInputs(const BoundExpressionInputs &) = delete;
	BoundExpressionInputs &operator=(const BoundExpressionInputs &) = delete;
	[[nodiscard]] const BoundExpressionInput *instruction(const expressions::Expression &expression) const {
		const auto found = instructions_.find(&expression);
		return found == instructions_.end() ? nullptr : &found->second;
	}
	class RowBinding final {
	public:
		RowBinding(BoundExpressionInputs &owner, const LogicalPlanReadRow &row) : owner_(owner) { owner_.bind(row); }
		~RowBinding() { owner_.unbind(); }
		RowBinding(const RowBinding &) = delete;
		RowBinding &operator=(const RowBinding &) = delete;
	private:
		BoundExpressionInputs &owner_;
	};
	[[nodiscard]] bool usesRow(const LogicalPlanReadRow &row) const noexcept { return row_ == &row; }
	[[nodiscard]] const PropertyValue *parameter(const BoundExpressionInput &input, const QueryContext *context) const {
		if (input.kind != BoundInputKind::BIK_PARAMETER || input.parameter_owner != parameter_identity_.row(0U).owner || context != context_)
			throw std::invalid_argument("parameter descriptor belongs to another query input owner");
		return parameters_.at(input.input_ordinal).value;
	}
	[[nodiscard]] const LogicalPlanReadValue *value(const BoundExpressionInput &input) const {
		if (!row_) throw std::logic_error("bound expression inputs have no active row");
		if (!input.source) return nullptr;
		(void) program_.name(*input.source);
		const auto layout = program_.view(op_).inputs;
		if (input.input_ordinal >= layout.size() || layout[input.input_ordinal] != *input.source)
			throw std::invalid_argument("bound expression descriptor belongs to another input layout");
		return values_.at(input.input_ordinal);
	}
	[[nodiscard]] std::optional<BoundPropertyView> property(const BoundExpressionInput &input,
		const storage::LogicalGraphReadSnapshot &snapshot) const {
		program_.validateSnapshot(snapshot);
		return input.property ? std::optional(properties_->view(*input.property, snapshot)) : std::nullopt;
	}
private:
	template<class Guard>
	void collect(const expressions::Expression *expression, std::pmr::vector<BoundPropertyRequest> &requests,
		std::pmr::vector<const expressions::Expression *> &properties, size_t depth, Guard &guard) {
		if (!expression) return;
		guard(); if (depth > 256U || ++visited_ > 65536U) throw std::runtime_error("bound expression work limit exceeded");
		if (instructions_.contains(expression)) return;
		const auto nested = [&](const expressions::Expression *child) { collect(child, requests, properties, depth + 1U, guard); };
		using expressions::ExpressionType;
		switch (expression->getExpressionType()) {
			case ExpressionType::PARAMETER: {
				const auto &name = static_cast<const expressions::ParameterExpression &>(*expression).getParameterName();
				size_t index = 0U;
				while (index < parameters_.size() && std::string_view(parameters_[index].name) != std::string_view(name)) ++index;
				if (index == parameters_.size()) {
					if (name.size() > 1024U * 1024U - parameter_name_bytes_) throw std::length_error("bound parameter name limit exceeded");
					const PropertyValue *value = nullptr;
					if (context_) { const auto found = context_->parameters.find(name); if (found != context_->parameters.end()) value = &found->second; }
					parameters_.emplace_back(name, value, &memory_); parameter_name_bytes_ += name.size();
				}
				BoundExpressionInput input; input.kind = BoundInputKind::BIK_PARAMETER; input.input_ordinal = static_cast<uint32_t>(index);
				input.parameter_owner = parameter_identity_.row(0U).owner; instructions_.emplace(expression, input); break;
			}
			case ExpressionType::VARIABLE_REFERENCE:
			case ExpressionType::PROPERTY_ACCESS: {
				const auto &reference = dynamic_cast<const expressions::VariableReferenceExpression &>(*expression);
				BoundExpressionInput input;
				const auto slots = program_.view(op_).inputs;
				for (size_t index = 0U; index < slots.size(); ++index) {
					const auto name = program_.name(slots[index]);
					if (reference.hasProperty() && name.size() > reference.getVariableName().size() &&
						name.starts_with(reference.getVariableName()) && name[reference.getVariableName().size()] == '.' &&
						name.substr(reference.getVariableName().size() + 1U) == reference.getPropertyName()) {
						input = {BoundInputKind::BIK_VALUE, slots[index], std::nullopt, static_cast<uint32_t>(index)}; break;
					}
				}
				if (!input.source) {
					input.source = program_.input(op_, reference.getVariableName());
					if (input.source) {
						input.input_ordinal = static_cast<uint32_t>(std::ranges::find(slots, *input.source) - slots.begin());
						input.kind = reference.hasProperty() ? BoundInputKind::BIK_PROPERTY : BoundInputKind::BIK_VALUE;
					}
				}
				instructions_.emplace(expression, input);
				if (input.kind == BoundInputKind::BIK_PROPERTY && !reference.getPropertyName().empty()) {
					requests.push_back({*input.source, reference.getPropertyName()}); properties.push_back(expression);
				}
				break;
			}
			case ExpressionType::BINARY_OP: {
				const auto &node = static_cast<const expressions::BinaryOpExpression &>(*expression); nested(node.getLeft()); nested(node.getRight()); break;
			}
			case ExpressionType::UNARY_OP: nested(static_cast<const expressions::UnaryOpExpression &>(*expression).getOperand()); break;
			case ExpressionType::IS_NULL: nested(static_cast<const expressions::IsNullExpression &>(*expression).getExpression()); break;
			case ExpressionType::IN_LIST: {
				const auto &node = static_cast<const expressions::InExpression &>(*expression); nested(node.getValue()); nested(node.getListExpression()); break;
			}
			case ExpressionType::FUNCTION_CALL:
				for (const auto &argument : static_cast<const expressions::FunctionCallExpression &>(*expression).getArguments()) nested(argument.get()); break;
			case ExpressionType::CASE_EXPRESSION: {
				const auto &node = static_cast<const expressions::CaseExpression &>(*expression); nested(node.getComparisonExpression());
				for (const auto &branch : node.getBranches()) { nested(branch.whenExpression.get()); nested(branch.thenExpression.get()); }
				nested(node.getElseExpression()); break;
			}
			case ExpressionType::LIST_SLICE: {
				const auto &node = static_cast<const expressions::ListSliceExpression &>(*expression); nested(node.getList()); nested(node.getStart()); nested(node.getEnd()); break;
			}
			case ExpressionType::EXPR_LIST_LITERAL:
				for (const auto &element : static_cast<const expressions::ListLiteralExpression &>(*expression).getElements()) nested(element.get()); break;
			case ExpressionType::MAP_PROJECTION: {
				for (const auto &item : static_cast<const expressions::MapProjectionExpression &>(*expression).getItems()) nested(item.expression.get()); break;
			}
			// Local bindings/patterns use the semantic evaluator's charged scope
			// adapter; do not recurse into their shadowing scope as if it were outer.
			default: break;
		}
	}
	void bind(const LogicalPlanReadRow &row) {
		if (row_) throw std::logic_error("bound input slots are already borrowed");
		// The old row map is traversed once at the declared migration boundary;
		// expression instructions subsequently read numeric input ordinals.
		for (const auto &[name, value] : row.values) {
			const auto slot = program_.input(op_, name);
			if (slot) values_[input_ordinals_.at(slot->index)] = &value;
		}
		row_ = &row;
	}
	void unbind() noexcept { std::fill(values_.begin(), values_.end(), nullptr); row_ = nullptr; }
	const BoundExecutionProgram &program_;
	BoundOperatorId op_;
	const QueryContext *context_;
	BatchIdentity parameter_identity_;
	struct Parameter {
		std::pmr::string name;
		const PropertyValue *value;
		Parameter(std::string_view key, const PropertyValue *source, std::pmr::memory_resource *memory) : name(key, memory), value(source) {}
	};
	BudgetMemoryResource memory_;
	std::pmr::unordered_map<const expressions::Expression *, BoundExpressionInput> instructions_;
	std::pmr::vector<const LogicalPlanReadValue *> values_;
	std::pmr::unordered_map<uint32_t, uint32_t> input_ordinals_;
	std::pmr::vector<Parameter> parameters_;
	std::optional<BoundPropertyBindings> properties_;
	const LogicalPlanReadRow *row_ = nullptr;
	size_t visited_ = 0U;
	size_t parameter_name_bytes_ = 0U;
};
}
