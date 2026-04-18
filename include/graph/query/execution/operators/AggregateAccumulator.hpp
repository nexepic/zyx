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
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace graph::query::execution::operators {

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
		if (graph::query::expressions::EvaluationContext::isNull(value)) {
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
		if (graph::query::expressions::EvaluationContext::isNull(value)) {
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
		if (graph::query::expressions::EvaluationContext::isNull(value)) {
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
		if (graph::query::expressions::EvaluationContext::isNull(value)) {
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
 * Collects all values into a LIST PropertyValue.
 */
class CollectAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		values_.push_back(value);
	}

	[[nodiscard]] PropertyValue getResult() const override {
		return PropertyValue(values_);
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
		if (!graph::query::expressions::EvaluationContext::isNull(value)) {
			seen_.insert(value.toString());
		}
	}

	[[nodiscard]] PropertyValue getResult() const override {
		return PropertyValue(static_cast<int64_t>(seen_.size()));
	}

	void reset() override {
		seen_.clear();
	}

	[[nodiscard]] std::unique_ptr<AggregateAccumulator> clone() const override {
		auto acc = std::make_unique<CountDistinctAccumulator>();
		acc->seen_ = seen_;
		return acc;
	}

private:
	std::unordered_set<std::string> seen_;
};

/**
 * @class CollectDistinctAccumulator
 * @brief Accumulator for collect(DISTINCT expr) function.
 */
class CollectDistinctAccumulator : public AggregateAccumulator {
public:
	void update(const PropertyValue& value) override {
		std::string key = value.toString();
		if (seen_.find(key) == seen_.end()) {
			seen_.insert(key);
			values_.push_back(value);
		}
	}

	[[nodiscard]] PropertyValue getResult() const override {
		return PropertyValue(values_);
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
	std::unordered_set<std::string> seen_;
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
		if (graph::query::expressions::EvaluationContext::isNull(value)) {
			return;
		}
		PropertyType type = value.getType();
		double x = 0.0;
		if (type == PropertyType::INTEGER) {
			x = static_cast<double>(std::get<int64_t>(value.getVariant()));
		} else if (type == PropertyType::DOUBLE) {
			x = std::get<double>(value.getVariant());
		} else {
			return;
		}
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
	explicit PercentileDiscAccumulator(double percentile) : percentile_(percentile) {}

	void update(const PropertyValue& value) override {
		if (!graph::query::expressions::EvaluationContext::isNull(value)) {
			values_.push_back(value);
		}
	}

	[[nodiscard]] PropertyValue getResult() const override {
		if (values_.empty()) return PropertyValue();
		auto sorted = values_;
		std::sort(sorted.begin(), sorted.end());
		size_t idx = static_cast<size_t>(std::floor(percentile_ * static_cast<double>(sorted.size() - 1)));
		return sorted[idx];
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
	double percentile_;
	std::vector<PropertyValue> values_;
};

/**
 * @class PercentileContAccumulator
 * @brief Accumulator for percentileCont() — materialize + sort + linear interpolation.
 */
class PercentileContAccumulator : public AggregateAccumulator {
public:
	explicit PercentileContAccumulator(double percentile) : percentile_(percentile) {}

	void update(const PropertyValue& value) override {
		if (graph::query::expressions::EvaluationContext::isNull(value)) {
			return;
		}
		PropertyType type = value.getType();
		if (type == PropertyType::INTEGER || type == PropertyType::DOUBLE) {
			values_.push_back(value);
		}
	}

	[[nodiscard]] PropertyValue getResult() const override {
		if (values_.empty()) return PropertyValue();
		auto sorted = values_;
		std::sort(sorted.begin(), sorted.end());
		double rank = percentile_ * static_cast<double>(sorted.size() - 1);
		size_t lo = static_cast<size_t>(std::floor(rank));
		size_t hi = static_cast<size_t>(std::ceil(rank));
		if (lo == hi || hi >= sorted.size()) {
			return sorted[lo];
		}
		double loVal = (sorted[lo].getType() == PropertyType::INTEGER)
			? static_cast<double>(std::get<int64_t>(sorted[lo].getVariant()))
			: std::get<double>(sorted[lo].getVariant());
		double hiVal = (sorted[hi].getType() == PropertyType::INTEGER)
			? static_cast<double>(std::get<int64_t>(sorted[hi].getVariant()))
			: std::get<double>(sorted[hi].getVariant());
		double frac = rank - static_cast<double>(lo);
		return PropertyValue(loVal + frac * (hiVal - loVal));
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
	double percentile_;
	std::vector<PropertyValue> values_;
};

/**
 * @brief Factory function to create an accumulator for a given aggregate function type.
 */
[[nodiscard]] inline std::unique_ptr<AggregateAccumulator> createAccumulator(AggregateFunctionType type, bool distinct = false, double percentileArg = 0.5) {
	switch (type) {
		case AggregateFunctionType::AGG_COUNT:
			if (distinct) return std::make_unique<CountDistinctAccumulator>();
			return std::make_unique<CountAccumulator>();
		case AggregateFunctionType::AGG_SUM:
			return std::make_unique<SumAccumulator>();
		case AggregateFunctionType::AGG_AVG:
			return std::make_unique<AvgAccumulator>();
		case AggregateFunctionType::AGG_MIN:
			return std::make_unique<MinAccumulator>();
		case AggregateFunctionType::AGG_MAX:
			return std::make_unique<MaxAccumulator>();
		case AggregateFunctionType::AGG_COLLECT:
			if (distinct) return std::make_unique<CollectDistinctAccumulator>();
			return std::make_unique<CollectAccumulator>();
		case AggregateFunctionType::AGG_STDEV:
			return std::make_unique<StDevAccumulator>(false);
		case AggregateFunctionType::AGG_STDEVP:
			return std::make_unique<StDevAccumulator>(true);
		case AggregateFunctionType::AGG_PERCENTILE_DISC:
			return std::make_unique<PercentileDiscAccumulator>(percentileArg);
		case AggregateFunctionType::AGG_PERCENTILE_CONT:
			return std::make_unique<PercentileContAccumulator>(percentileArg);
		default:
			return nullptr;
	}
}

} // namespace graph::query::execution::operators
