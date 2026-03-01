/**
 * @file AggregateAccumulator.hpp
 * @author Metrix Contributors
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
#include <memory>
#include <string>
#include <vector>
#include <variant>

namespace graph::query::execution::operators {

/**
 * @enum AggregateFunctionType
 * @brief Types of aggregate functions supported.
 */
enum class AggregateFunctionType {
	COUNT,
	SUM,
	AVG,
	MIN,
	MAX,
	COLLECT
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

	/**
	 * @brief Returns the final aggregated result.
	 * @return The aggregated value
	 */
	[[nodiscard]] virtual PropertyValue getResult() const = 0;

	/**
	 * @brief Resets the accumulator to its initial state.
	 */
	virtual void reset() = 0;

	/**
	 * @brief Creates a copy of the accumulator.
	 */
	[[nodiscard]] virtual std::unique_ptr<AggregateAccumulator> clone() const = 0;
};

/**
 * @class CountAccumulator
 * @brief Accumulator for COUNT() function.
 */
class CountAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		// COUNT counts all non-NULL values
		if (!graph::query::expressions::EvaluationContext::isNull(value)) {
			count_++;
		}
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
	size_t count_ = 0;
};

/**
 * @class SumAccumulator
 * @brief Accumulator for SUM() function.
 */
class SumAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		// Skip NULL values
		if (!graph::query::expressions::EvaluationContext::isNull(value)) {
			return;
		}

		PropertyType type = value.getType();
		if (type == PropertyType::INTEGER) {
			sumInt_ += std::get<int64_t>(value.getVariant());
			hasInt_ = true;
		} else if (type == PropertyType::DOUBLE) {
			sumDouble_ += std::get<double>(value.getVariant());
			hasDouble_ = true;
		}
	}

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
		if (!graph::query::expressions::EvaluationContext::isNull(value)) {
			return;
		}

		PropertyType type = value.getType();
		if (type == PropertyType::INTEGER) {
			sum_ += static_cast<double>(std::get<int64_t>(value.getVariant()));
			count_++;
		} else if (type == PropertyType::DOUBLE) {
			sum_ += std::get<double>(value.getVariant());
			count_++;
		}
	}

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
	void update(const PropertyValue& value) override {
		// Skip NULL values
		if (!graph::query::expressions::EvaluationContext::isNull(value)) {
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

	[[nodiscard]] PropertyValue getResult() const override {
		if (!hasValue_) {
			return PropertyValue(); // NULL if no values
		}
		return minValue_;
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
	void update(const PropertyValue& value) override {
		// Skip NULL values
		if (!graph::query::expressions::EvaluationContext::isNull(value)) {
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

	[[nodiscard]] PropertyValue getResult() const override {
		if (!hasValue_) {
			return PropertyValue(); // NULL if no values
		}
		return maxValue_;
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
 * Note: Since PropertyValue doesn't support LIST type with arbitrary elements,
 * this implementation stores values internally and returns a string representation.
 */
class CollectAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		values_.push_back(value);
	}

	[[nodiscard]] PropertyValue getResult() const override {
		// For now, return a comma-separated string representation
		// In a full implementation, we'd need to extend PropertyValue to support lists
		std::string result = "[";
		for (size_t i = 0; i < values_.size(); ++i) {
			if (i > 0) result += ", ";
			result += values_[i].toString();
		}
		result += "]";
		return PropertyValue(result);
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
 * @brief Factory function to create an accumulator for a given aggregate function type.
 */
[[nodiscard]] inline std::unique_ptr<AggregateAccumulator> createAccumulator(AggregateFunctionType type) {
	switch (type) {
		case AggregateFunctionType::COUNT:
			return std::make_unique<CountAccumulator>();
		case AggregateFunctionType::SUM:
			return std::make_unique<SumAccumulator>();
		case AggregateFunctionType::AVG:
			return std::make_unique<AvgAccumulator>();
		case AggregateFunctionType::MIN:
			return std::make_unique<MinAccumulator>();
		case AggregateFunctionType::MAX:
			return std::make_unique<MaxAccumulator>();
		case AggregateFunctionType::COLLECT:
			return std::make_unique<CollectAccumulator>();
		default:
			return nullptr;
	}
}

} // namespace graph::query::execution::operators
