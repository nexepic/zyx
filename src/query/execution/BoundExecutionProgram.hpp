#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <unordered_set>
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "query/execution/BoundSlotRegistry.hpp"

namespace graph::query::execution::detail {

struct BoundOperatorId {
	uint32_t index;
	uint64_t program;
	bool operator==(const BoundOperatorId &) const = default;
};
enum class BoundOperatorMode { BOM_ROW_ADAPTER, BOM_SCALAR_INPUTS, BOM_BLOCKING_INPUTS };
struct BoundProgramLimits {
	size_t operators = 4096U, slots = 65536U, name_bytes = 1024U * 1024U, depth = 256U;
};

// Immutable query-local topology/layout owner. ASTs and the pinned reader must
// outlive it. Every legacy row boundary is explicit; this object is never cached.
class BoundExecutionProgram final {
public:
	struct OperatorView {
		const logical::LogicalOperator *source;
		BoundScopeId inputs_scope, outputs_scope;
		std::span<const BoundOperatorId> children;
		std::span<const BoundSlotId> inputs, outputs;
		BoundOperatorMode mode;
		std::span<const BoundSlotId> declared_outputs;
	};
	template<class Guard>
	BoundExecutionProgram(const logical::LogicalOperator &root, const storage::LogicalGraphReadSnapshot &snapshot,
		MaterializedByteBudget &budget, Guard guard, BoundProgramLimits limits = {},
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource(),
		std::span<const std::string_view> injectedNames = {})
		: limits_(limits), registry_(limits.slots, checkedScopeCount(limits.operators), limits.name_bytes, budget, upstream),
		  memory_(budget, upstream), nodes_(&memory_), sources_(&memory_), active_(&memory_), budget_(budget),
		  schema_(snapshot.bindingSchemaIdentity()), tokens_(snapshot.supportsPropertyTokenResolution()) {
		if (limits.operators == 0U || limits.operators > UINT32_MAX || limits.depth == 0U || limits.depth > 256U)
			throw std::invalid_argument("bound program requires bounded operators and recursion");
		if (schema_ && !schema_->isValid()) throw std::invalid_argument("bound program received an invalid schema identity");
		std::pmr::vector<std::pmr::string> imports(&memory_);
		for (const auto name : injectedNames) {
			guard(); if (name.empty()) throw std::invalid_argument("injected input has an empty name");
			imports.emplace_back(name);
		}
		root_ = compile(root, imports, 1U, guard);
		registry_.seal(); active_.clear();
		guard(); validateSnapshot(snapshot);
	}
	BoundExecutionProgram(const BoundExecutionProgram &) = delete;
	BoundExecutionProgram &operator=(const BoundExecutionProgram &) = delete;
	[[nodiscard]] BoundOperatorId root() const noexcept { return root_; }
	[[nodiscard]] size_t size() const noexcept { return nodes_.size(); }
	[[nodiscard]] size_t slotCount() const noexcept { return registry_.slotCount(); }
	[[nodiscard]] const BoundSlotRegistry &slots() const noexcept { return registry_; }
	[[nodiscard]] BoundOperatorId id(const logical::LogicalOperator &source) const {
		const auto found = sources_.find(&source);
		if (found == sources_.end()) throw std::invalid_argument("operator belongs to another bound program");
		return {found->second, registry_.root().plan};
	}
	[[nodiscard]] OperatorView view(BoundOperatorId descriptor) const {
		if (descriptor.program != registry_.root().plan || descriptor.index >= nodes_.size())
			throw std::invalid_argument("foreign or invalid bound operator descriptor");
		const auto &node = nodes_[descriptor.index];
		return {node.source, node.inputs_scope, node.outputs_scope, node.children, node.inputs, node.outputs, node.mode, node.declared_outputs};
	}
	[[nodiscard]] std::optional<BoundSlotId> input(BoundOperatorId op, std::string_view name) const {
		return registry_.resolve(view(op).inputs_scope, name);
	}
	[[nodiscard]] std::string_view name(BoundSlotId slot) const { return registry_.name(slot); }
	void validateSnapshot(const storage::LogicalGraphReadSnapshot &snapshot) const {
		if (snapshot.bindingSchemaIdentity() != schema_ || snapshot.supportsPropertyTokenResolution() != tokens_)
			throw std::runtime_error("bound execution program reader identity changed; rebind required");
	}

private:
	struct Node {
		Node(const logical::LogicalOperator *op, BoundScopeId inputsScope, BoundScopeId outputsScope,
			std::pmr::memory_resource *memory) : source(op), inputs_scope(inputsScope), outputs_scope(outputsScope),
			children(memory), inputs(memory), outputs(memory), declared_outputs(memory) {}
		const logical::LogicalOperator *source;
		BoundScopeId inputs_scope, outputs_scope;
		std::pmr::vector<BoundOperatorId> children;
		std::pmr::vector<BoundSlotId> inputs, outputs, declared_outputs;
		BoundOperatorMode mode = BoundOperatorMode::BOM_ROW_ADAPTER;
	};
	static size_t checkedScopeCount(size_t operators) {
		if (operators > (UINT32_MAX - 1U) / 2U) throw std::invalid_argument("bound program scope count overflow");
		return 1U + operators * 2U;
	}
	void appendName(Node &node, std::string_view name, bool output) {
		if (name.empty()) throw std::invalid_argument("bound operator has an empty variable");
		const auto scope = output ? node.outputs_scope : node.inputs_scope;
		if (registry_.resolve(scope, name)) return; // Shared join aliases map to one destination slot.
		const std::pmr::string stableName(name, &memory_);
		const auto slot = registry_.declare(scope, stableName);
		(output ? node.outputs : node.inputs).push_back(slot);
	}
	template<class Guard>
	BoundOperatorId compile(const logical::LogicalOperator &source,
		const std::pmr::vector<std::pmr::string> &imports, size_t depth, Guard &guard) {
		guard();
		if (depth > limits_.depth || nodes_.size() == limits_.operators)
			throw std::runtime_error("bound program exceeded its operator/depth work limit");
		if (active_.contains(&source)) throw std::invalid_argument("logical operator graph contains a cycle");
		if (sources_.contains(&source)) throw std::invalid_argument("logical operator graph reuses an owned child");
		active_.insert(&source);
		const auto ordinal = static_cast<uint32_t>(nodes_.size());
		const BoundOperatorId descriptor{ordinal, registry_.root().plan};
		const auto inputScope = registry_.createScope(registry_.root(), BindingScopeVisibility::BSV_ISOLATED);
		const auto outputScope = registry_.createScope(registry_.root(), BindingScopeVisibility::BSV_ISOLATED);
		nodes_.emplace_back(&source, inputScope, outputScope, &memory_);
		sources_.emplace(&source, ordinal);
		for (const auto &name : imports) appendName(nodes_[ordinal], name, false);
		// These two existing IR APIs return owning std containers. Account their
		// temporary capacity at this declared compiler adapter boundary.
		MaterializedByteReservation temporary(&budget_);
		const auto children = source.getChildren();
		if ((source.getType() == logical::LogicalOpType::LOP_NODE_SCAN || source.getType() == logical::LogicalOpType::LOP_SINGLE_ROW) && !children.empty())
			throw std::invalid_argument("bound leaf operator unexpectedly has a child");
		temporary.set(children.capacity() * sizeof(logical::LogicalOperator *));
		const auto *subquery = dynamic_cast<const logical::LogicalCallSubquery *>(&source);
		for (const auto *child : children) {
			if (!child) throw std::invalid_argument("logical operator has a null child");
			std::pmr::vector<std::pmr::string> childImports(&memory_);
			for (const auto &name : imports) childImports.emplace_back(name);
			if (subquery && child == subquery->getSubquery()) {
				childImports.clear();
				for (const auto &name : subquery->getImportedVars()) childImports.emplace_back(name);
			}
			const auto childId = compile(*child, childImports, depth + 1U, guard);
			nodes_[ordinal].children.push_back(childId);
			if (!subquery || child == subquery->getInput()) {
				for (const auto slot : nodes_[childId.index].outputs)
					appendName(nodes_[ordinal], registry_.name(slot), false);
			}
		}
		const auto outputs = source.getOutputVariables();
		size_t bytes = children.capacity() * sizeof(logical::LogicalOperator *) + outputs.capacity() * sizeof(std::string);
		for (const auto &name : outputs) {
			if (name.capacity() > (SIZE_MAX - bytes) - 1U) throw std::overflow_error("bound IR adapter name size overflow");
			bytes += name.capacity() + 1U;
		}
		temporary.set(bytes);
		for (const auto &name : outputs) {
			guard(); if (declared_output_count_ == limits_.slots) throw std::length_error("bound program declared output limit exceeded");
			appendName(nodes_[ordinal], name, true);
			nodes_[ordinal].declared_outputs.push_back(*registry_.resolve(outputScope, name));
			++declared_output_count_;
		}
		if (children.empty()) for (const auto &name : outputs) appendName(nodes_[ordinal], name, false);
		if (subquery) {
			for (const auto &name : subquery->getImportedVars())
				if (!registry_.resolve(inputScope, name)) throw std::invalid_argument("correlated import is absent from its input layout");
		}
		switch (source.getType()) {
			case logical::LogicalOpType::LOP_FILTER:
			case logical::LogicalOpType::LOP_PROJECT: nodes_[ordinal].mode = BoundOperatorMode::BOM_SCALAR_INPUTS; break;
			case logical::LogicalOpType::LOP_AGGREGATE:
			case logical::LogicalOpType::LOP_SORT:
			case logical::LogicalOpType::LOP_JOIN: nodes_[ordinal].mode = BoundOperatorMode::BOM_BLOCKING_INPUTS; break;
			default: break;
		}
		guard(); active_.erase(&source); return descriptor;
	}
	BoundProgramLimits limits_;
	BoundSlotRegistry registry_;
	BudgetMemoryResource memory_;
	std::pmr::vector<Node> nodes_;
	std::pmr::unordered_map<const logical::LogicalOperator *, uint32_t> sources_;
	std::pmr::unordered_set<const logical::LogicalOperator *> active_;
	MaterializedByteBudget &budget_;
	std::optional<storage::LogicalGraphSchemaIdentity> schema_;
	bool tokens_;
	BoundOperatorId root_{};
	size_t declared_output_count_ = 0U;
};
}
