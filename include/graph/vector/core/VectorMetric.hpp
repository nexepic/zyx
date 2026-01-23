/**
 * @file VectorMetric.hpp
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
#include <simsimd/simsimd.h>
#include "BFloat16.hpp"

namespace graph::vector {

	class VectorMetric {
	public:
		// Compute L2 Squared distance between two BFloat16 vectors using hardware acceleration
		static float computeL2Sqr(const BFloat16 *a, const BFloat16 *b, size_t dim) {
			simsimd_distance_t result;
			simsimd_l2sq_bf16(reinterpret_cast<const simsimd_bf16_t *>(a), reinterpret_cast<const simsimd_bf16_t *>(b),
							  dim, &result);
			return static_cast<float>(result);
		}

		// Compute Inner Product using hardware acceleration
		static float computeIP(const BFloat16 *a, const BFloat16 *b, size_t dim) {
			simsimd_distance_t result;
			simsimd_dot_bf16(reinterpret_cast<const simsimd_bf16_t *>(a), reinterpret_cast<const simsimd_bf16_t *>(b),
							 dim, &result);
			return static_cast<float>(result);
		}

		// Mixed Precision: Float32 Query vs BFloat16 Target
		// Fallback to manual loop as SimSIMD mixed-precision support varies by version
		static float computeL2Sqr(const float *query, const BFloat16 *target, size_t dim) {
			float dist = 0.0f;
			for (size_t i = 0; i < dim; ++i) {
				float diff = query[i] - target[i].toFloat();
				dist += diff * diff;
			}
			return dist;
		}
	};

} // namespace graph::vector
