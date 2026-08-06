/**
 * @file LogicalPropertyPredicate.cpp
 * @brief Typed predicate evaluation used by the native logical graph runtime.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/query/execution/VectorizedPredicate.hpp"

#include <type_traits>

namespace graph::query::execution {

	namespace {

		template <typename T>
		[[nodiscard]] const T *asValue(const PropertyValue &value) {
			return std::get_if<T>(&value.getVariant());
		}

		template <typename T>
		[[nodiscard]] bool compare(const T &actual, VectorPredicateOp op, const T &bound) {
			switch (op) {
				case VectorPredicateOp::VPO_EQ: return actual == bound;
				case VectorPredicateOp::VPO_NE: return actual != bound;
				case VectorPredicateOp::VPO_LT: return actual < bound;
				case VectorPredicateOp::VPO_LE: return actual <= bound;
				case VectorPredicateOp::VPO_GT: return actual > bound;
				case VectorPredicateOp::VPO_GE: return actual >= bound;
				case VectorPredicateOp::VPO_RANGE_CLOSED: return false;
			}
			return false;
		}

		template <typename T>
		[[nodiscard]] std::optional<bool> typedComparison(const PropertyValue &actual,
															const VectorizedPropertyPredicate &predicate) {
			const T *actualValue = asValue<T>(actual);
			const T *boundValue = asValue<T>(predicate.value);
			if (actualValue == nullptr || boundValue == nullptr) return std::nullopt;
			return compare(*actualValue, predicate.op, *boundValue);
		}

		template <typename T>
		[[nodiscard]] std::optional<bool> typedRange(const PropertyValue &actual,
															const VectorizedPropertyPredicate &predicate) {
			if (!predicate.upperValue.has_value()) return std::nullopt;
			const T *actualValue = asValue<T>(actual);
			const T *lowerValue = asValue<T>(predicate.value);
			const T *upperValue = asValue<T>(*predicate.upperValue);
			if (actualValue == nullptr || lowerValue == nullptr || upperValue == nullptr) return std::nullopt;
			return *actualValue >= *lowerValue && *actualValue <= *upperValue;
		}

		[[nodiscard]] bool genericComparison(const PropertyValue &actual, const VectorizedPropertyPredicate &predicate) {
			switch (predicate.op) {
				case VectorPredicateOp::VPO_EQ: return actual == predicate.value;
				case VectorPredicateOp::VPO_NE: return actual != predicate.value;
				case VectorPredicateOp::VPO_LT: return actual < predicate.value;
				case VectorPredicateOp::VPO_LE: return actual <= predicate.value;
				case VectorPredicateOp::VPO_GT: return actual > predicate.value;
				case VectorPredicateOp::VPO_GE: return actual >= predicate.value;
				case VectorPredicateOp::VPO_RANGE_CLOSED:
					return predicate.upperValue.has_value() && actual >= predicate.value && actual <= *predicate.upperValue;
			}
			return false;
		}

		template <typename Function>
		[[nodiscard]] std::optional<bool> firstTypedMatch(Function &&function) {
			if (auto result = function.template operator()<int64_t>()) return result;
			if (auto result = function.template operator()<double>()) return result;
			if (auto result = function.template operator()<bool>()) return result;
			if (auto result = function.template operator()<std::string>()) return result;
			if (auto result = function.template operator()<TemporalDate>()) return result;
			if (auto result = function.template operator()<TemporalDateTime>()) return result;
			return function.template operator()<TemporalDuration>();
		}

	} // namespace

	bool evaluatePredicateValue(const std::optional<PropertyValue> &actual,
									const VectorizedPropertyPredicate &predicate) {
		if (!actual.has_value()) return false;
		if (predicate.op == VectorPredicateOp::VPO_RANGE_CLOSED) {
			if (auto result = firstTypedMatch([&]<typename T> { return typedRange<T>(*actual, predicate); })) {
				return *result;
			}
		} else if (auto result = firstTypedMatch([&]<typename T> { return typedComparison<T>(*actual, predicate); })) {
			return *result;
		}
		return genericComparison(*actual, predicate);
	}

} // namespace graph::query::execution
