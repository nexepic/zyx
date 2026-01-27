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

#include "BFloat16.hpp"

namespace graph::vector {

	class VectorMetric {
	public:
		// ====================================================================
		// L2 Squared Distance (Euclidean)
		// ====================================================================

		/**
		 * @brief Compute L2 Squared distance between two Float32 vectors.
		 * Used by KMeans and Product Quantizer training.
		 * Optimized with 4-way unrolling.
		 */
		static float computeL2Sqr(const float *a, const float *b, size_t dim) {
			float sum = 0.0f;
			size_t i = 0;

			// Unrolling loop to help compiler auto-vectorization
			// and reduce loop overhead.
			for (; i + 4 <= dim; i += 4) {
				float d0 = a[i] - b[i];
				float d1 = a[i + 1] - b[i + 1];
				float d2 = a[i + 2] - b[i + 2];
				float d3 = a[i + 3] - b[i + 3];

				sum += d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
			}

			// Handle remaining elements
			for (; i < dim; ++i) {
				float d = a[i] - b[i];
				sum += d * d;
			}
			return sum;
		}

		/**
		 * @brief Compute L2 Squared distance: Float32 Query vs BFloat16 Target.
		 * Used during Search (Mixed Precision).
		 */
		static float computeL2Sqr(const float *query, const BFloat16 *target, size_t dim) {
			float sum = 0.0f;
			size_t i = 0;

			for (; i + 4 <= dim; i += 4) {
				float d0 = query[i] - target[i].toFloat();
				float d1 = query[i + 1] - target[i + 1].toFloat();
				float d2 = query[i + 2] - target[i + 2].toFloat();
				float d3 = query[i + 3] - target[i + 3].toFloat();

				sum += d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
			}

			for (; i < dim; ++i) {
				float d = query[i] - target[i].toFloat();
				sum += d * d;
			}
			return sum;
		}

		/**
		 * @brief Compute L2 Squared distance: BFloat16 vs BFloat16.
		 * Used for internal graph maintenance.
		 */
		static float computeL2Sqr(const BFloat16 *a, const BFloat16 *b, size_t dim) {
			float sum = 0.0f;
			size_t i = 0;

			for (; i < dim; ++i) {
				float d = a[i].toFloat() - b[i].toFloat();
				sum += d * d;
			}
			return sum;
		}

		// ====================================================================
		// Inner Product (Cosine Similarity related)
		// Note: Returns NEGATIVE IP so that smaller value = closer distance.
		// ====================================================================

		/**
		 * @brief Compute Inner Product: Float32 vs Float32.
		 */
		static float computeIP(const float *a, const float *b, size_t dim) {
			float sum = 0.0f;
			size_t i = 0;

			for (; i + 4 <= dim; i += 4) {
				sum += a[i] * b[i] + a[i + 1] * b[i + 1] + a[i + 2] * b[i + 2] + a[i + 3] * b[i + 3];
			}

			for (; i < dim; ++i) {
				sum += a[i] * b[i];
			}
			return -sum; // Negate for distance sorting
		}

		/**
		 * @brief Compute Inner Product: Float32 vs BFloat16.
		 */
		static float computeIP(const float *a, const BFloat16 *b, size_t dim) {
			float sum = 0.0f;
			for (size_t i = 0; i < dim; ++i) {
				sum += a[i] * b[i].toFloat();
			}
			return -sum;
		}

		/**
		 * @brief Compute Inner Product: BFloat16 vs BFloat16.
		 */
		static float computeIP(const BFloat16 *a, const BFloat16 *b, size_t dim) {
			float sum = 0.0f;
			for (size_t i = 0; i < dim; ++i) {
				sum += a[i].toFloat() * b[i].toFloat();
			}
			return -sum;
		}
	};

} // namespace graph::vector
