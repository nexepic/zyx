#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/NodeColumnBatch.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	class PropertyPredicateKernel {
	public:
		explicit PropertyPredicateKernel(std::vector<VectorizedPropertyPredicate> predicates);

		[[nodiscard]] static PropertyPredicateKernel
		fromEqualityPredicates(const std::unordered_map<std::string, PropertyValue> &predicates);

		[[nodiscard]] bool empty() const noexcept { return predicates_.empty(); }
		[[nodiscard]] const std::vector<VectorizedPropertyPredicate> &predicates() const noexcept { return predicates_; }

		[[nodiscard]] bool matchesValue(const std::optional<PropertyValue> &actual,
		                               const VectorizedPropertyPredicate &predicate) const;
		[[nodiscard]] bool matchesMap(const std::unordered_map<std::string, PropertyValue> &properties) const;
		[[nodiscard]] std::vector<storage::PropertyEntityPredicate> toStoragePredicates() const;

		void apply(NodeColumnBatch &batch) const;

	private:
		std::vector<VectorizedPropertyPredicate> predicates_;
	};

	[[nodiscard]] bool evaluatePredicateWithKernel(const std::optional<PropertyValue> &actual,
	                                             const VectorizedPropertyPredicate &predicate);

} // namespace graph::query::execution
