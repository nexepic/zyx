/**
 * @file AggregateAccumulator.hpp
 * @author ZYX Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/execution/TypedDistinctSet.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <functional>
#include "graph/query/execution/GuardedPropertyCopy.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace graph::query::execution {

inline size_t checkedAggregateBytesAdd(size_t left, size_t right) {
	if (right > std::numeric_limits<size_t>::max() - left) {
		throw std::overflow_error("logical aggregate memory accounting overflowed");
	}
	return left + right;
}

inline size_t checkedAggregateBytesMul(size_t left, size_t right) {
	if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
		throw std::overflow_error("logical aggregate memory accounting overflowed");
	}
	return left * right;
}

/**
 * @enum AggregateFunctionType
 * @brief Types of aggregate functions supported.
 */
enum class AggregateFunctionType {
	AGG_COUNT,
	AGG_SUM,
	AGG_AVG,
	AGG_MIN,
	AGG_MAX,
	AGG_COLLECT,
	AGG_STDEV,
	AGG_STDEVP,
	AGG_PERCENTILE_DISC,
	AGG_PERCENTILE_CONT
};

/**
 * @class AggregateAccumulator
 * @brief Base class for aggregation accumulators.
 *
 * Each aggregate function (COUNT, SUM, AVG, MIN, MAX, collect()) has its own
 * accumulator that maintains state during the aggregation process.
 */
class AggregateAccumulator {
public:
	virtual ~AggregateAccumulator() = default;

	/**
	 * @brief Updates the accumulator with a new value.
	 * @param value The value to add to the accumulation
	 * @param context The evaluation context for expression evaluation
	 */
	virtual void update(const PropertyValue& value) = 0;
	virtual void updateAdmitted(const PropertyValue &value, const std::function<void(size_t)> &) { update(value); }


	/**
	 * @brief Updates the accumulator for COUNT(*)-style row counting.
	 */
	virtual void updateRow() {
		update(PropertyValue(static_cast<int64_t>(1)));
	}

	/**
	 * @brief Returns the final aggregated result.
	 * @return The aggregated value
	 */
	[[nodiscard]] virtual PropertyValue getResult() const = 0;

	// Destructive finalization for owners that transfer a completed group.
	// getResult() remains available for callers requiring a reusable snapshot.
	[[nodiscard]] virtual PropertyValue takeResult() {
		PropertyValue result = getResult();
		reset();
		return result;
	}

	// Bytes retained by the accumulator after its last update. This is a
	// payload estimate used by the logical runtime's aggregate memory guard;
	// scalar accumulators normally retain no user-owned payload.
	[[nodiscard]] virtual size_t retainedBytes() const { return 0U; }
	// Excludes storage already charged by an explicitly supplied resource.
	[[nodiscard]] virtual size_t retainedBytesForAdmission() const { return retainedBytes(); }


	/**
	 * @brief Resets the accumulator to its initial state.
	 */
	virtual void reset() = 0;

	/**
	 * @brief Creates a copy of the accumulator.
	 */
	[[nodiscard]] virtual std::unique_ptr<AggregateAccumulator> clone() const = 0;
};

inline void validateNumericAggregateInput(const PropertyValue &value, std::string_view functionName) {
	if (value.getType() != PropertyType::INTEGER && value.getType() != PropertyType::DOUBLE) {
		throw std::invalid_argument(std::string(functionName) + " requires a numeric input");
	}
}

/**
 * @class CountAccumulator
 * @brief Accumulator for COUNT() function.
 */
class CountAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		// COUNT counts all non-NULL values
		if (value.getType() != PropertyType::NULL_TYPE) {
			addRows(1U);
		}
	}

	void updateRow() override {
		addRows(1U);
	}

    // A partial COUNT may merge a complete admitted batch in one operation.
    void addRows(uint64_t rows) {
        if (rows > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - count_)
            throw std::overflow_error("COUNT exceeded the signed result range");
        count_ += rows;
    }

	[[nodiscard]] PropertyValue getResult() const override {
		return PropertyValue(static_cast<int64_t>(count_));
	}

	void reset() override {
		count_ = 0;
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<CountAccumulator>();
		acc->count_ = count_;
		return acc;
	}

private:
	uint64_t count_ = 0;
};

/**
 * @class SumAccumulator
 * @brief Accumulator for SUM() function.
 */
class SumAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		// Skip NULL values
		if (value.getType() == PropertyType::NULL_TYPE) {
			return;
		}

		validateNumericAggregateInput(value, "sum()");
		PropertyType type = value.getType();
		if (type == PropertyType::INTEGER) {
			updateInteger(std::get<int64_t>(value.getVariant()));
		} else {
			updateDouble(std::get<double>(value.getVariant()));
		}
	}

	void updateInteger(int64_t input) {
		// Check before addition, including when a floating sibling has already
		// contributed. Failed updates leave both partial sums unchanged.
		if ((input > 0 && sumInt_ > std::numeric_limits<int64_t>::max() - input) ||
			(input < 0 && sumInt_ < std::numeric_limits<int64_t>::min() - input)) {
			throw std::overflow_error("sum() integer accumulation overflowed");
		}
		sumInt_ += input;
		hasInt_ = true;
	}
	void updateDouble(double input) { sumDouble_ += input; hasDouble_ = true; }

	[[nodiscard]] PropertyValue getResult() const override {
		if (hasDouble_) {
			return PropertyValue(sumDouble_ + static_cast<double>(sumInt_));
		} else if (hasInt_) {
			return PropertyValue(sumInt_);
		}
		return PropertyValue(); // NULL if no values
	}

	void reset() override {
		sumInt_ = 0;
		sumDouble_ = 0.0;
		hasInt_ = false;
		hasDouble_ = false;
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<SumAccumulator>();
		acc->sumInt_ = sumInt_;
		acc->sumDouble_ = sumDouble_;
		acc->hasInt_ = hasInt_;
		acc->hasDouble_ = hasDouble_;
		return acc;
	}

private:
	int64_t sumInt_ = 0;
	double sumDouble_ = 0.0;
	bool hasInt_ = false;
	bool hasDouble_ = false;
};

/**
 * @class AvgAccumulator
 * @brief Accumulator for AVG() function.
 */
class AvgAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		// Skip NULL values
		if (value.getType() == PropertyType::NULL_TYPE) {
			return;
		}

		validateNumericAggregateInput(value, "avg()");
		PropertyType type = value.getType();
		if (type == PropertyType::INTEGER) {
			updateInteger(std::get<int64_t>(value.getVariant()));
		} else {
			updateDouble(std::get<double>(value.getVariant()));
		}
	}
	void updateInteger(int64_t input) { updateDouble(static_cast<double>(input)); }
	void updateDouble(double input) { sum_ += input; ++count_; }

	[[nodiscard]] PropertyValue getResult() const override {
		if (count_ == 0) {
			return PropertyValue(); // NULL if no values
		}
		return PropertyValue(sum_ / static_cast<double>(count_));
	}

	void reset() override {
		sum_ = 0.0;
		count_ = 0;
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<AvgAccumulator>();
		acc->sum_ = sum_;
		acc->count_ = count_;
		return acc;
	}

private:
	double sum_ = 0.0;
	size_t count_ = 0;
};

/**
 * @class MinAccumulator
 * @brief Accumulator for MIN() function.
 */
class MinAccumulator : public AggregateAccumulator {
public:
	void updateInteger(int64_t input) {
		if (hasValue_ && minValue_.getType() == PropertyType::INTEGER) {
			if (input < std::get<int64_t>(minValue_.getVariant())) minValue_ = PropertyValue(input);
		} else update(PropertyValue(input));
	}
	void updateDouble(double input) {
		if (hasValue_ && minValue_.getType() == PropertyType::DOUBLE) {
			if (input < std::get<double>(minValue_.getVariant())) minValue_ = PropertyValue(input);
		} else update(PropertyValue(input));
	}
	void update(const PropertyValue& value) override {
		// Skip NULL values
		if (value.getType() == PropertyType::NULL_TYPE) {
			return;
		}

		if (!hasValue_) {
			minValue_ = value;
			hasValue_ = true;
		} else {
			if (value < minValue_) {
				minValue_ = value;
			}
		}
	}

	void updateAdmitted(const PropertyValue &value, const std::function<void(size_t)> &admit) override {
		if (value.getType() == PropertyType::NULL_TYPE || (hasValue_ && !(value < minValue_))) return;
		const auto bytes = detail::retainedPropertyValueBytes(value, [&] { admit(0U); });
		admit(bytes);
		PropertyValue copied = detail::copyPropertyValueGuarded(value, [&] { admit(bytes); }, true);
		minValue_ = std::move(copied); hasValue_ = true;
	}

	[[nodiscard]] PropertyValue getResult() const override {
		if (!hasValue_) {
			return PropertyValue(); // NULL if no values
		}
		return minValue_;
	}
	[[nodiscard]] PropertyValue takeResult() override {
		if (!hasValue_) return PropertyValue();
		PropertyValue result = std::move(minValue_);
		reset();
		return result;
	}
	[[nodiscard]] size_t retainedBytes() const override {
		return hasValue_ ? detail::retainedPropertyValueBytes(minValue_, [] {}) : 0U;
	}

	void reset() override {
		minValue_ = PropertyValue();
		hasValue_ = false;
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<MinAccumulator>();
		acc->minValue_ = minValue_;
		acc->hasValue_ = hasValue_;
		return acc;
	}

private:
	PropertyValue minValue_;
	bool hasValue_ = false;
};

/**
 * @class MaxAccumulator
 * @brief Accumulator for MAX() function.
 */
class MaxAccumulator : public AggregateAccumulator {
public:
	void updateInteger(int64_t input) {
		if (hasValue_ && maxValue_.getType() == PropertyType::INTEGER) {
			if (input > std::get<int64_t>(maxValue_.getVariant())) maxValue_ = PropertyValue(input);
		} else update(PropertyValue(input));
	}
	void updateDouble(double input) {
		if (hasValue_ && maxValue_.getType() == PropertyType::DOUBLE) {
			if (input > std::get<double>(maxValue_.getVariant())) maxValue_ = PropertyValue(input);
		} else update(PropertyValue(input));
	}
	void update(const PropertyValue& value) override {
		// Skip NULL values
		if (value.getType() == PropertyType::NULL_TYPE) {
			return;
		}

		if (!hasValue_) {
			maxValue_ = value;
			hasValue_ = true;
		} else {
			if (value > maxValue_) {
				maxValue_ = value;
			}
		}
	}

	void updateAdmitted(const PropertyValue &value, const std::function<void(size_t)> &admit) override {
		if (value.getType() == PropertyType::NULL_TYPE || (hasValue_ && !(value > maxValue_))) return;
		const auto bytes = detail::retainedPropertyValueBytes(value, [&] { admit(0U); });
		admit(bytes);
		PropertyValue copied = detail::copyPropertyValueGuarded(value, [&] { admit(bytes); }, true);
		maxValue_ = std::move(copied); hasValue_ = true;
	}

	[[nodiscard]] PropertyValue getResult() const override {
		if (!hasValue_) {
			return PropertyValue(); // NULL if no values
		}
		return maxValue_;
	}
	[[nodiscard]] PropertyValue takeResult() override {
		if (!hasValue_) return PropertyValue();
		PropertyValue result = std::move(maxValue_);
		reset();
		return result;
	}
	[[nodiscard]] size_t retainedBytes() const override {
		return hasValue_ ? detail::retainedPropertyValueBytes(maxValue_, [] {}) : 0U;
	}

	void reset() override {
		maxValue_ = PropertyValue();
		hasValue_ = false;
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<MaxAccumulator>();
		acc->maxValue_ = maxValue_;
		acc->hasValue_ = hasValue_;
		return acc;
	}

private:
	PropertyValue maxValue_;
	bool hasValue_ = false;
};

/**
 * @class CollectAccumulator
 * @brief Accumulator for collect() function.
 *
 * Collects all values into a LIST PropertyValue.
 */
class CollectAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		if (value.getType() == PropertyType::NULL_TYPE) {
			return;
		}
		values_.push_back(value);
	}

	void updateAdmitted(const PropertyValue &value, const std::function<void(size_t)> &admit) override {
		if (value.getType() == PropertyType::NULL_TYPE) return;
		const auto payload = detail::retainedPropertyValueBytes(value, [&] { admit(0U); });
		const bool grow = values_.size() == values_.capacity();
		const size_t capacity = grow ? (values_.capacity() ? checkedAggregateBytesMul(values_.capacity(), 2U) : 1U) : values_.capacity();
		const size_t peak = checkedAggregateBytesAdd(payload, grow ? checkedAggregateBytesMul(capacity, sizeof(PropertyValue)) : 0U);
		admit(peak);
		PropertyValue copied = detail::copyPropertyValueGuarded(value, [&] { admit(peak); }, true);
		if (grow) values_.reserve(capacity);
		values_.push_back(std::move(copied));
	}

	[[nodiscard]] PropertyValue getResult() const override {
		return PropertyValue(values_);
	}
	[[nodiscard]] PropertyValue takeResult() override {
		PropertyValue result(std::move(values_));
		std::vector<PropertyValue>().swap(values_);
		reset();
		return result;
	}
	[[nodiscard]] size_t retainedBytes() const override {
		size_t total = checkedAggregateBytesMul(values_.capacity(), sizeof(PropertyValue));
		for (const PropertyValue &value: values_) {
			total = checkedAggregateBytesAdd(total, detail::retainedPropertyValueBytes(value, [] {}));
		}
		return total;
	}

	void reset() override {
		values_.clear();
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<CollectAccumulator>();
		acc->values_ = values_;
		return acc;
	}

private:
	std::vector<PropertyValue> values_;
};

/**
 * @class CountDistinctAccumulator
 * @brief Accumulator for COUNT(DISTINCT expr) function.
 */
class CountDistinctAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		if (value.getType() != PropertyType::NULL_TYPE) {
			seen_.insert(value);
		}
	}

	[[nodiscard]] PropertyValue getResult() const override {
		return PropertyValue(static_cast<int64_t>(seen_.size()));
	}
	[[nodiscard]] size_t retainedBytes() const override { return seen_.memoryBytes(); }

	void reset() override {
		seen_.clear();
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<CountDistinctAccumulator>();
		acc->seen_ = seen_;
		return acc;
	}

private:
	TypedDistinctSet seen_;
};

/**
 * @class CollectDistinctAccumulator
 * @brief Accumulator for collect(DISTINCT expr) function.
 */
class CollectDistinctAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		updateAdmitted(value, [](size_t) {});
	}

	void updateAdmitted(const PropertyValue &value, const std::function<void(size_t)> &admit) override {
		if (value.getType() == PropertyType::NULL_TYPE || seen_.contains(value)) return;
		const size_t payload = detail::retainedPropertyValueBytes(value, [&] { admit(0U); });
		const bool grow = values_.size() == values_.capacity();
		const size_t capacity = grow ? (values_.capacity() ? checkedAggregateBytesMul(values_.capacity(), 2U) : 1U) : values_.capacity();
		const size_t peak = checkedAggregateBytesAdd(checkedAggregateBytesMul(payload, 2U),
			grow ? checkedAggregateBytesMul(capacity, sizeof(PropertyValue)) : 0U);
		admit(peak);
		PropertyValue output = detail::copyPropertyValueGuarded(value, [&] { admit(peak); }, true);
		const auto type = value.getType();
		const bool fallback = type == PropertyType::DURATION || type == PropertyType::BYTES ||
			type == PropertyType::LIST || type == PropertyType::MAP || type == PropertyType::COMPOSITE || type == PropertyType::UNKNOWN;
		PropertyValue distinct;
		if (fallback) distinct = detail::copyPropertyValueGuarded(value, [&] { admit(peak); }, true);
		if (grow) values_.reserve(capacity);
		values_.push_back(std::move(output));
		try {
			if (!(fallback ? seen_.insertFallbackOwned(std::move(distinct)) : seen_.insert(value))) values_.pop_back();
		} catch (...) {
			values_.pop_back();
			throw;
		}
	}

	[[nodiscard]] PropertyValue getResult() const override {
		return PropertyValue(values_);
	}
	[[nodiscard]] PropertyValue takeResult() override {
		PropertyValue result(std::move(values_));
		std::vector<PropertyValue>().swap(values_);
		reset();
		return result;
	}
	[[nodiscard]] size_t retainedBytes() const override {
		size_t total = checkedAggregateBytesAdd(
				checkedAggregateBytesMul(values_.capacity(), sizeof(PropertyValue)), seen_.memoryBytes());
		for (const PropertyValue &value: values_) {
			total = checkedAggregateBytesAdd(total, detail::retainedPropertyValueBytes(value, [] {}));
		}
		return total;
	}

	void reset() override {
		seen_.clear();
		values_.clear();
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<CollectDistinctAccumulator>();
		acc->seen_ = seen_;
		acc->values_ = values_;
		return acc;
	}

private:
	TypedDistinctSet seen_;
	std::vector<PropertyValue> values_;
};

/**
 * @class StDevAccumulator
 * @brief Accumulator for stDev()/stDevP() using Welford's online algorithm.
 */
class StDevAccumulator : public AggregateAccumulator {
public:
	explicit StDevAccumulator(bool population) : population_(population) {}

	void update(const PropertyValue& value) override {
		if (value.getType() == PropertyType::NULL_TYPE) {
			return;
		}
		validateNumericAggregateInput(value, "stdev()");
		if (const auto *integer = std::get_if<int64_t>(&value.getVariant())) updateInteger(*integer);
		else updateDouble(std::get<double>(value.getVariant()));
	}

	void updateInteger(int64_t value) { updateDouble(static_cast<double>(value)); }

	void updateDouble(double x) {
		n_++;
		double delta = x - mean_;
		mean_ += delta / static_cast<double>(n_);
		double delta2 = x - mean_;
		m2_ += delta * delta2;
	}

	[[nodiscard]] PropertyValue getResult() const override {
		if (population_) {
			if (n_ < 1) return PropertyValue();
			return PropertyValue(std::sqrt(m2_ / static_cast<double>(n_)));
		}
		if (n_ < 2) return PropertyValue();
		return PropertyValue(std::sqrt(m2_ / static_cast<double>(n_ - 1)));
	}

	void reset() override {
		n_ = 0;
		mean_ = 0.0;
		m2_ = 0.0;
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<StDevAccumulator>(population_);
		acc->n_ = n_;
		acc->mean_ = mean_;
		acc->m2_ = m2_;
		return acc;
	}

private:
	bool population_;
	size_t n_ = 0;
	double mean_ = 0.0;
	double m2_ = 0.0;
};

/**
 * @class PercentileDiscAccumulator
 * @brief Accumulator for percentileDisc() — materialize + sort, pick nearest rank.
 */
class PercentileDiscAccumulator : public AggregateAccumulator {
public:
	explicit PercentileDiscAccumulator(double percentile, std::pmr::memory_resource *memory = nullptr)
		: percentile_(percentile), externally_accounted_(memory != nullptr),
		  values_(memory ? memory : std::pmr::new_delete_resource()) {}

	void update(const PropertyValue& value) override {
		if (value.getType() == PropertyType::NULL_TYPE) return;
		validateNumericAggregateInput(value, "percentileDisc()");
		values_.push_back(value);
	}

	[[nodiscard]] PropertyValue getResult() const override {
		auto sorted = values_;
		std::sort(sorted.begin(), sorted.end());
		return resultFromSorted(sorted);
	}
	[[nodiscard]] PropertyValue takeResult() override {
		std::sort(values_.begin(), values_.end());
		PropertyValue result = resultFromSorted(values_);
		std::pmr::vector<PropertyValue>(values_.get_allocator()).swap(values_);
		return result;
	}
	[[nodiscard]] size_t retainedBytes() const override {
		size_t total = checkedAggregateBytesMul(values_.capacity(), sizeof(PropertyValue));
		for (const PropertyValue &value: values_) {
			total = checkedAggregateBytesAdd(total, property_utils::getPropertyValueSize(value));
		}
		return total;
	}

	[[nodiscard]] size_t retainedBytesForAdmission() const override {
		// update accepts only inline numeric values; the resource owns all
		// dynamic storage, including overlapping arrays during growth.
		return externally_accounted_ ? 0U : retainedBytes();
	}

	void reset() override {
		values_.clear();
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<PercentileDiscAccumulator>(percentile_);
		acc->values_ = values_;
		return acc;
	}

private:
	[[nodiscard]] PropertyValue resultFromSorted(std::span<const PropertyValue> values) const {
		if (values.empty()) return PropertyValue();
		size_t idx = static_cast<size_t>(std::floor(percentile_ * static_cast<double>(values.size() - 1)));
		return values[idx];
	}
	double percentile_;
	bool externally_accounted_;
	std::pmr::vector<PropertyValue> values_;
};

/**
 * @class PercentileContAccumulator
 * @brief Accumulator for percentileCont() — materialize + sort + linear interpolation.
 */
class PercentileContAccumulator : public AggregateAccumulator {
public:
	explicit PercentileContAccumulator(double percentile, std::pmr::memory_resource *memory = nullptr)
		: percentile_(percentile), externally_accounted_(memory != nullptr),
		  values_(memory ? memory : std::pmr::new_delete_resource()) {}

	void update(const PropertyValue& value) override {
		if (value.getType() == PropertyType::NULL_TYPE) {
			return;
		}
		validateNumericAggregateInput(value, "percentileCont()");
		values_.push_back(value);
	}

	[[nodiscard]] PropertyValue getResult() const override {
		auto sorted = values_;
		std::sort(sorted.begin(), sorted.end());
		return resultFromSorted(sorted);
	}
	[[nodiscard]] PropertyValue takeResult() override {
		std::sort(values_.begin(), values_.end());
		PropertyValue result = resultFromSorted(values_);
		std::pmr::vector<PropertyValue>(values_.get_allocator()).swap(values_);
		return result;
	}
	[[nodiscard]] size_t retainedBytes() const override {
		size_t total = checkedAggregateBytesMul(values_.capacity(), sizeof(PropertyValue));
		for (const PropertyValue &value: values_) {
			total = checkedAggregateBytesAdd(total, property_utils::getPropertyValueSize(value));
		}
		return total;
	}

	[[nodiscard]] size_t retainedBytesForAdmission() const override {
		// update accepts only inline numeric values; the resource owns all
		// dynamic storage, including overlapping arrays during growth.
		return externally_accounted_ ? 0U : retainedBytes();
	}

	void reset() override {
		values_.clear();
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<PercentileContAccumulator>(percentile_);
		acc->values_ = values_;
		return acc;
	}

private:
	[[nodiscard]] PropertyValue resultFromSorted(std::span<const PropertyValue> values) const {
		if (values.empty()) return PropertyValue();
		double rank = percentile_ * static_cast<double>(values.size() - 1);
		size_t lo = static_cast<size_t>(std::floor(rank));
		size_t hi = static_cast<size_t>(std::ceil(rank));
		if (lo == hi || hi >= values.size()) {
			return values[lo];
		}
		double loVal = (values[lo].getType() == PropertyType::INTEGER)
			? static_cast<double>(std::get<int64_t>(values[lo].getVariant()))
			: std::get<double>(values[lo].getVariant());
		double hiVal = (values[hi].getType() == PropertyType::INTEGER)
			? static_cast<double>(std::get<int64_t>(values[hi].getVariant()))
			: std::get<double>(values[hi].getVariant());
		double frac = rank - static_cast<double>(lo);
		return PropertyValue(loVal + frac * (hiVal - loVal));
	}
	double percentile_;
	bool externally_accounted_;
	std::pmr::vector<PropertyValue> values_;
};

/**
 * @brief Factory function to create an accumulator for a given aggregate function type.
 */
[[nodiscard]] inline std::unique_ptr<AggregateAccumulator> createAccumulator(AggregateFunctionType type, bool distinct = false, double percentileArg = 0.5, std::pmr::memory_resource *memory = nullptr,
	const std::function<void(size_t)> &admitObject = {}) {
	if ((type == AggregateFunctionType::AGG_PERCENTILE_DISC ||
		 type == AggregateFunctionType::AGG_PERCENTILE_CONT) &&
		(!std::isfinite(percentileArg) || percentileArg < 0.0 || percentileArg > 1.0)) {
		throw std::invalid_argument("percentile aggregate requires a finite value between zero and one");
	}
	// Charge the concrete object before allocating it. Payloads remain accounted
	// by each accumulator or its supplied memory resource.
	const auto make = [&]<typename T, typename... Args>(Args &&...args) -> std::unique_ptr<AggregateAccumulator> {
		if (admitObject) admitObject(sizeof(T));
		return std::make_unique<T>(std::forward<Args>(args)...);
	};

	switch (type) {
		case AggregateFunctionType::AGG_COUNT:
			if (distinct) return make.template operator()<CountDistinctAccumulator>();
			return make.template operator()<CountAccumulator>();
		case AggregateFunctionType::AGG_SUM:
			return make.template operator()<SumAccumulator>();
		case AggregateFunctionType::AGG_AVG:
			return make.template operator()<AvgAccumulator>();
		case AggregateFunctionType::AGG_MIN:
			return make.template operator()<MinAccumulator>();
		case AggregateFunctionType::AGG_MAX:
			return make.template operator()<MaxAccumulator>();
		case AggregateFunctionType::AGG_COLLECT:
			if (distinct) return make.template operator()<CollectDistinctAccumulator>();
			return make.template operator()<CollectAccumulator>();
		case AggregateFunctionType::AGG_STDEV:
			return make.template operator()<StDevAccumulator>(false);
		case AggregateFunctionType::AGG_STDEVP:
			return make.template operator()<StDevAccumulator>(true);
		case AggregateFunctionType::AGG_PERCENTILE_DISC:
			return make.template operator()<PercentileDiscAccumulator>(percentileArg, memory);
		case AggregateFunctionType::AGG_PERCENTILE_CONT:
			return make.template operator()<PercentileContAccumulator>(percentileArg, memory);
		default:
			return nullptr;
	}
}

} // namespace graph::query::execution
