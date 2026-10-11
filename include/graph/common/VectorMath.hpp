/**
 * @file VectorMath.hpp
 * @brief Native common BF16 conversion and Vector distance primitives.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace graph::common {

/**
 * @brief Brain floating-point 16-bit value using the upper IEEE-754 float bits.
 *
 * This is the canonical native on-disk conversion. It intentionally preserves
 * the established truncation semantics so persisted Vector bytes remain stable.
 */
struct alignas(2) BFloat16 {
	uint16_t data = 0U;

	BFloat16() = default;

	explicit BFloat16(const float value) {
		uint32_t bits = 0U;
		std::memcpy(&bits, &value, sizeof(value));
		data = static_cast<uint16_t>(bits >> 16U);
	}

	[[nodiscard]] float toFloat() const {
		const uint32_t bits = static_cast<uint32_t>(data) << 16U;
		float value = 0.0F;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}
};

[[nodiscard]] inline std::vector<BFloat16> toBFloat16(const std::vector<float> &values) {
	std::vector<BFloat16> result;
	result.reserve(values.size());
	for (const float value : values) {
		result.emplace_back(value);
	}
	return result;
}

[[nodiscard]] inline std::vector<float> toFloat(const std::vector<BFloat16> &values) {
	std::vector<float> result;
	result.reserve(values.size());
	for (const BFloat16 &value : values) {
		result.push_back(value.toFloat());
	}
	return result;
}

/** @brief Stateless distance primitives used by native Vector maintenance. */
class VectorMetric final {
public:
	[[nodiscard]] static float computeL2Sqr(const float *left, const float *right, const size_t dimensions) {
		float sum = 0.0F;
		size_t index = 0U;
		for (; index + 4U <= dimensions; index += 4U) {
			const float d0 = left[index] - right[index];
			const float d1 = left[index + 1U] - right[index + 1U];
			const float d2 = left[index + 2U] - right[index + 2U];
			const float d3 = left[index + 3U] - right[index + 3U];
			sum += d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
		}
		for (; index < dimensions; ++index) {
			const float difference = left[index] - right[index];
			sum += difference * difference;
		}
		return sum;
	}

	[[nodiscard]] static float computeL2Sqr(const float *query, const BFloat16 *target, const size_t dimensions) {
		float sum = 0.0F;
		size_t index = 0U;
		for (; index + 4U <= dimensions; index += 4U) {
			const float d0 = query[index] - target[index].toFloat();
			const float d1 = query[index + 1U] - target[index + 1U].toFloat();
			const float d2 = query[index + 2U] - target[index + 2U].toFloat();
			const float d3 = query[index + 3U] - target[index + 3U].toFloat();
			sum += d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
		}
		for (; index < dimensions; ++index) {
			const float difference = query[index] - target[index].toFloat();
			sum += difference * difference;
		}
		return sum;
	}

	[[nodiscard]] static float computeL2Sqr(const BFloat16 *left, const BFloat16 *right, const size_t dimensions) {
		float sum = 0.0F;
		size_t index = 0U;
		for (; index + 4U <= dimensions; index += 4U) {
			const float d0 = left[index].toFloat() - right[index].toFloat();
			const float d1 = left[index + 1U].toFloat() - right[index + 1U].toFloat();
			const float d2 = left[index + 2U].toFloat() - right[index + 2U].toFloat();
			const float d3 = left[index + 3U].toFloat() - right[index + 3U].toFloat();
			sum += d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
		}
		for (; index < dimensions; ++index) {
			const float difference = left[index].toFloat() - right[index].toFloat();
			sum += difference * difference;
		}
		return sum;
	}

	[[nodiscard]] static float computeIP(const float *left, const float *right, const size_t dimensions) {
		float sum = 0.0F;
		size_t index = 0U;
		for (; index + 4U <= dimensions; index += 4U) {
			sum += left[index] * right[index] + left[index + 1U] * right[index + 1U] +
				   left[index + 2U] * right[index + 2U] + left[index + 3U] * right[index + 3U];
		}
		for (; index < dimensions; ++index) {
			sum += left[index] * right[index];
		}
		return -sum;
	}

	[[nodiscard]] static float computeIP(const float *left, const BFloat16 *right, const size_t dimensions) {
		float sum = 0.0F;
		size_t index = 0U;
		for (; index + 4U <= dimensions; index += 4U) {
			sum += left[index] * right[index].toFloat() + left[index + 1U] * right[index + 1U].toFloat() +
				   left[index + 2U] * right[index + 2U].toFloat() + left[index + 3U] * right[index + 3U].toFloat();
		}
		for (; index < dimensions; ++index) {
			sum += left[index] * right[index].toFloat();
		}
		return -sum;
	}

	[[nodiscard]] static float computeIP(const BFloat16 *left, const BFloat16 *right, const size_t dimensions) {
		float sum = 0.0F;
		size_t index = 0U;
		for (; index + 4U <= dimensions; index += 4U) {
			sum += left[index].toFloat() * right[index].toFloat() +
				   left[index + 1U].toFloat() * right[index + 1U].toFloat() +
				   left[index + 2U].toFloat() * right[index + 2U].toFloat() +
				   left[index + 3U].toFloat() * right[index + 3U].toFloat();
		}
		for (; index < dimensions; ++index) {
			sum += left[index].toFloat() * right[index].toFloat();
		}
		return -sum;
	}
};

} // namespace graph::common
