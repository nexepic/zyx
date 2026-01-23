/**
 * @file BFloat16.hpp
 * @author Nexepic
 * @date 2026/1/21
 *
 * @copyright Copyright (c) 2026 Nexepic
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
#include <vector>

namespace graph::vector {

	/**
	 * @brief Brain Floating Point 16-bit.
	 * Layout matches IEEE 754 float32's upper 16 bits.
	 */
	struct alignas(2) BFloat16 {
		uint16_t data;

		BFloat16() = default;

		// Fast truncation conversion from float
		explicit BFloat16(float v) {
			uint32_t f_bits;
			std::memcpy(&f_bits, &v, sizeof(float));
			data = static_cast<uint16_t>(f_bits >> 16);
		}

		[[nodiscard]] float toFloat() const {
			uint32_t f_bits = static_cast<uint32_t>(data) << 16;
			float v;
			std::memcpy(&v, &f_bits, sizeof(float));
			return v;
		}
	};

	inline std::vector<BFloat16> toBFloat16(const std::vector<float> &vec) {
		std::vector<BFloat16> res;
		res.reserve(vec.size());
		for (float v: vec)
			res.emplace_back(v);
		return res;
	}

	inline std::vector<float> toFloat(const std::vector<BFloat16> &vec) {
		std::vector<float> res;
		res.reserve(vec.size());
		for (const auto &v: vec)
			res.push_back(v.toFloat());
		return res;
	}

} // namespace graph::vector
