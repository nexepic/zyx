#include "graph/query/execution/PropertyPredicateKernel.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

namespace graph::query::execution {
namespace {

	storage::PropertyEntityPredicateOp toStoragePredicateOp(VectorPredicateOp op) {
		switch (op) {
			case VectorPredicateOp::VPO_EQ:
				return storage::PropertyEntityPredicateOp::PEP_EQ;
			case VectorPredicateOp::VPO_NE:
				return storage::PropertyEntityPredicateOp::PEP_NE;
			case VectorPredicateOp::VPO_LT:
				return storage::PropertyEntityPredicateOp::PEP_LT;
			case VectorPredicateOp::VPO_LE:
				return storage::PropertyEntityPredicateOp::PEP_LE;
			case VectorPredicateOp::VPO_GT:
				return storage::PropertyEntityPredicateOp::PEP_GT;
			case VectorPredicateOp::VPO_GE:
				return storage::PropertyEntityPredicateOp::PEP_GE;
			case VectorPredicateOp::VPO_RANGE_CLOSED:
				return storage::PropertyEntityPredicateOp::PEP_RANGE_CLOSED;
		}
		return storage::PropertyEntityPredicateOp::PEP_EQ;
	}

	template <typename T>
	const T *asValue(const PropertyValue &value) {
		return std::get_if<T>(&value.getVariant());
	}

	template <typename T>
	bool evaluateTypedComparison(const T &actual, VectorPredicateOp op, const T &bound) {
		switch (op) {
			case VectorPredicateOp::VPO_EQ:
				return actual == bound;
			case VectorPredicateOp::VPO_NE:
				return actual != bound;
			case VectorPredicateOp::VPO_LT:
				return actual < bound;
			case VectorPredicateOp::VPO_LE:
				return actual <= bound;
			case VectorPredicateOp::VPO_GT:
				return actual > bound;
			case VectorPredicateOp::VPO_GE:
				return actual >= bound;
			case VectorPredicateOp::VPO_RANGE_CLOSED:
				break;
		}
		return false;
	}

	template <typename T>
	std::optional<bool> tryTypedComparison(const PropertyValue &actual,
	                                      const VectorizedPropertyPredicate &predicate) {
		const auto *actualValue = asValue<T>(actual);
		const auto *boundValue = asValue<T>(predicate.value);
		if (actualValue == nullptr || boundValue == nullptr) {
			return std::nullopt;
		}
		return evaluateTypedComparison(*actualValue, predicate.op, *boundValue);
	}

	template <typename T>
	std::optional<bool> tryTypedRange(const PropertyValue &actual,
	                                 const VectorizedPropertyPredicate &predicate) {
		const auto *actualValue = asValue<T>(actual);
		const auto *lowerValue = asValue<T>(predicate.value);
		if (actualValue == nullptr || lowerValue == nullptr || !predicate.upperValue.has_value()) {
			return std::nullopt;
		}
		const auto *upperValue = asValue<T>(*predicate.upperValue);
		if (upperValue == nullptr) {
			return std::nullopt;
		}
		return *actualValue >= *lowerValue && *actualValue <= *upperValue;
	}

	bool evaluateGenericPredicate(const PropertyValue &actual, const VectorizedPropertyPredicate &predicate) {
		switch (predicate.op) {
			case VectorPredicateOp::VPO_EQ:
				return actual == predicate.value;
			case VectorPredicateOp::VPO_NE:
				return actual != predicate.value;
			case VectorPredicateOp::VPO_LT:
				return actual < predicate.value;
			case VectorPredicateOp::VPO_LE:
				return actual <= predicate.value;
			case VectorPredicateOp::VPO_GT:
				return actual > predicate.value;
			case VectorPredicateOp::VPO_GE:
				return actual >= predicate.value;
			case VectorPredicateOp::VPO_RANGE_CLOSED:
				return predicate.upperValue.has_value() && actual >= predicate.value && actual <= *predicate.upperValue;
		}
		return false;
	}

	std::optional<bool> tryTypedPredicate(const PropertyValue &actual, const VectorizedPropertyPredicate &predicate) {
		if (predicate.op == VectorPredicateOp::VPO_RANGE_CLOSED) {
			if (auto result = tryTypedRange<int64_t>(actual, predicate)) {
				return result;
			}
			if (auto result = tryTypedRange<double>(actual, predicate)) {
				return result;
			}
			if (auto result = tryTypedRange<std::string>(actual, predicate)) {
				return result;
			}
			if (auto result = tryTypedRange<TemporalDate>(actual, predicate)) {
				return result;
			}
			if (auto result = tryTypedRange<TemporalDateTime>(actual, predicate)) {
				return result;
			}
			if (auto result = tryTypedRange<TemporalDuration>(actual, predicate)) {
				return result;
			}
			return std::nullopt;
		}

		if (auto result = tryTypedComparison<int64_t>(actual, predicate)) {
			return result;
		}
		if (auto result = tryTypedComparison<double>(actual, predicate)) {
			return result;
		}
		if (auto result = tryTypedComparison<bool>(actual, predicate)) {
			return result;
		}
		if (auto result = tryTypedComparison<std::string>(actual, predicate)) {
			return result;
		}
		if (auto result = tryTypedComparison<TemporalDate>(actual, predicate)) {
			return result;
		}
		if (auto result = tryTypedComparison<TemporalDateTime>(actual, predicate)) {
			return result;
		}
		if (auto result = tryTypedComparison<TemporalDuration>(actual, predicate)) {
			return result;
		}
		return std::nullopt;
	}

} // namespace

	PropertyPredicateKernel::PropertyPredicateKernel(std::vector<VectorizedPropertyPredicate> predicates)
		: predicates_(std::move(predicates)) {}

	PropertyPredicateKernel PropertyPredicateKernel::fromEqualityPredicates(
			const std::unordered_map<std::string, PropertyValue> &predicates) {
		std::vector<VectorizedPropertyPredicate> vectorPredicates;
		vectorPredicates.reserve(predicates.size());
		for (const auto &[key, value] : predicates) {
			VectorizedPropertyPredicate predicate;
			predicate.propertyKey = key;
			predicate.op = VectorPredicateOp::VPO_EQ;
			predicate.value = value;
			vectorPredicates.push_back(std::move(predicate));
		}
		return PropertyPredicateKernel(std::move(vectorPredicates));
	}

	bool PropertyPredicateKernel::containsOnlyEqualityPredicates() const {
		return !predicates_.empty() && std::all_of(predicates_.begin(), predicates_.end(), [](const auto &predicate) {
			return predicate.op == VectorPredicateOp::VPO_EQ;
		});
	}

	std::unordered_map<std::string, PropertyValue> PropertyPredicateKernel::toEqualityPredicates() const {
		std::unordered_map<std::string, PropertyValue> equality;
		equality.reserve(predicates_.size());
		for (const auto &predicate : predicates_) {
			equality[predicate.propertyKey] = predicate.value;
		}
		return equality;
	}

	bool PropertyPredicateKernel::matchesValue(const std::optional<PropertyValue> &actual,
	                                        const VectorizedPropertyPredicate &predicate) const {
		if (!actual.has_value()) {
			return false;
		}
		if (auto typed = tryTypedPredicate(*actual, predicate)) {
			return *typed;
		}
		return evaluateGenericPredicate(*actual, predicate);
	}

	bool PropertyPredicateKernel::matchesMap(const std::unordered_map<std::string, PropertyValue> &properties) const {
		for (const auto &predicate : predicates_) {
			const auto it = properties.find(predicate.propertyKey);
			const auto actual = it == properties.end() ? std::optional<PropertyValue>{}
			                                      : std::optional<PropertyValue>{it->second};
			if (!matchesValue(actual, predicate)) {
				return false;
			}
		}
		return true;
	}

	std::vector<storage::PropertyEntityPredicate> PropertyPredicateKernel::toStoragePredicates() const {
		std::vector<storage::PropertyEntityPredicate> storagePredicates;
		storagePredicates.reserve(predicates_.size());
		for (const auto &predicate : predicates_) {
			storage::PropertyEntityPredicate storagePredicate;
			storagePredicate.key = predicate.propertyKey;
			storagePredicate.op = toStoragePredicateOp(predicate.op);
			storagePredicate.value = predicate.value;
			storagePredicate.upperValue = predicate.upperValue;
			storagePredicates.push_back(std::move(storagePredicate));
		}
		return storagePredicates;
	}

	void PropertyPredicateKernel::apply(NodeColumnBatch &batch) const {
		batch.ensureSelectionVector();
		for (const auto &predicate : predicates_) {
			auto columnIt = batch.propertyColumns.find(predicate.propertyKey);
			if (columnIt == batch.propertyColumns.end()) {
				std::fill(batch.selected.begin(), batch.selected.end(), uint8_t{0});
				return;
			}

			const auto &column = columnIt->second;
			for (size_t rowIndex = 0; rowIndex < batch.nodeIds.size(); ++rowIndex) {
				if (batch.selected[rowIndex] == 0) {
					continue;
				}
				const auto actual = rowIndex < column.size() ? column[rowIndex] : std::optional<PropertyValue>{};
				if (!matchesValue(actual, predicate)) {
					batch.selected[rowIndex] = 0;
				}
			}
		}
	}

	bool evaluatePredicateWithKernel(const std::optional<PropertyValue> &actual,
	                              const VectorizedPropertyPredicate &predicate) {
		PropertyPredicateKernel kernel({predicate});
		return kernel.matchesValue(actual, predicate);
	}

} // namespace graph::query::execution
