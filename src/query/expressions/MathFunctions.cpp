/**
 * @file MathFunctions.cpp
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

#include "BuiltinFunctions.hpp"
#include <cmath>
#include <random>

namespace graph::query::expressions {

static PropertyValue absImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}

	const PropertyValue& arg = args[0];
	PropertyType type = arg.getType();

	if (type == PropertyType::INTEGER) {
		int64_t val = std::get<int64_t>(arg.getVariant());
		return PropertyValue(std::abs(val));
	} else if (type == PropertyType::DOUBLE) {
		double val = std::get<double>(arg.getVariant());
		return PropertyValue(std::fabs(val));
	}

	double val = EvaluationContext::toDouble(arg);
	return PropertyValue(std::fabs(val));
}

static PropertyValue ceilImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::ceil(val));
}

static PropertyValue floorImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::floor(val));
}

static PropertyValue roundImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::round(val));
}

static PropertyValue sqrtImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val < 0.0) {
		return PropertyValue();
	}
	return PropertyValue(std::sqrt(val));
}

static PropertyValue signImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val > 0.0) {
		return PropertyValue(static_cast<int64_t>(1));
	} else if (val < 0.0) {
		return PropertyValue(static_cast<int64_t>(-1));
	} else {
		return PropertyValue(static_cast<int64_t>(0));
	}
}

static PropertyValue logImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val <= 0.0) {
		return PropertyValue();
	}
	return PropertyValue(std::log(val));
}

static PropertyValue log10Impl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val <= 0.0) {
		return PropertyValue();
	}
	return PropertyValue(std::log10(val));
}

static PropertyValue expImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	return PropertyValue(std::exp(val));
}

static PropertyValue powImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}
	double base = EvaluationContext::toDouble(args[0]);
	double exponent = EvaluationContext::toDouble(args[1]);
	return PropertyValue(std::pow(base, exponent));
}

static PropertyValue sinImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	return PropertyValue(std::sin(EvaluationContext::toDouble(args[0])));
}

static PropertyValue cosImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	return PropertyValue(std::cos(EvaluationContext::toDouble(args[0])));
}

static PropertyValue tanImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	return PropertyValue(std::tan(EvaluationContext::toDouble(args[0])));
}

static PropertyValue asinImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val < -1.0 || val > 1.0) {
		return PropertyValue();
	}
	return PropertyValue(std::asin(val));
}

static PropertyValue acosImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	double val = EvaluationContext::toDouble(args[0]);
	if (val < -1.0 || val > 1.0) {
		return PropertyValue();
	}
	return PropertyValue(std::acos(val));
}

static PropertyValue atanImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) {
		return PropertyValue();
	}
	return PropertyValue(std::atan(EvaluationContext::toDouble(args[0])));
}

static PropertyValue atan2Impl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.size() < 2 || EvaluationContext::isNull(args[0]) || EvaluationContext::isNull(args[1])) {
		return PropertyValue();
	}
	double y = EvaluationContext::toDouble(args[0]);
	double x = EvaluationContext::toDouble(args[1]);
	return PropertyValue(std::atan2(y, x));
}

static PropertyValue randImpl(
	[[maybe_unused]] const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	static thread_local std::mt19937_64 gen(std::random_device{}());
	std::uniform_real_distribution<double> dist(0.0, 1.0);
	return PropertyValue(dist(gen));
}

static constexpr double PI_CONSTANT = 3.14159265358979323846;
static constexpr double E_CONSTANT = 2.71828182845904523536;

static PropertyValue piImpl(
	[[maybe_unused]] const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	return PropertyValue(PI_CONSTANT);
}

static PropertyValue eConstImpl(
	[[maybe_unused]] const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	return PropertyValue(E_CONSTANT);
}

void registerMathFunctions(FunctionRegistry& registry) {
	registry.registerFunction(makeFn("abs", 1, 1, &absImpl));
	registry.registerFunction(makeFn("ceil", 1, 1, &ceilImpl));
	registry.registerFunction(makeFn("floor", 1, 1, &floorImpl));
	registry.registerFunction(makeFn("round", 1, 1, &roundImpl));
	registry.registerFunction(makeFn("sqrt", 1, 1, &sqrtImpl));
	registry.registerFunction(makeFn("sign", 1, 1, &signImpl));
	registry.registerFunction(makeFn("log", 1, 1, &logImpl));
	registry.registerFunction(makeFn("log10", 1, 1, &log10Impl));
	registry.registerFunction(makeFn("exp", 1, 1, &expImpl));
	registry.registerFunction(makeFn("pow", 2, 2, &powImpl));
	registry.registerFunction(makeFn("sin", 1, 1, &sinImpl));
	registry.registerFunction(makeFn("cos", 1, 1, &cosImpl));
	registry.registerFunction(makeFn("tan", 1, 1, &tanImpl));
	registry.registerFunction(makeFn("asin", 1, 1, &asinImpl));
	registry.registerFunction(makeFn("acos", 1, 1, &acosImpl));
	registry.registerFunction(makeFn("atan", 1, 1, &atanImpl));
	registry.registerFunction(makeFn("atan2", 2, 2, &atan2Impl));
	registry.registerFunction(makeFn("rand", 0, 0, &randImpl));
	registry.registerFunction(makeFn("pi", 0, 0, &piImpl));
	registry.registerFunction(makeFn("e", 0, 0, &eConstImpl));
}

} // namespace graph::query::expressions
