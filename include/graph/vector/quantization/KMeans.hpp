/**
 * @file KMeans.hpp
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

#include <limits>
#include <random>
#include <simsimd/simsimd.h>
#include <vector>

namespace graph::vector {
	class KMeans {
	public:
		static std::vector<std::vector<float>> run(const std::vector<std::vector<float>> &data, size_t k,
												   size_t max_iter = 15) {

			if (data.empty())
				return {};
			size_t dim = data[0].size();
			size_t n = data.size();

			std::vector centroids(k, std::vector<float>(dim));
			std::vector<int> assignment(n);

			std::mt19937 rng(42);
			std::uniform_int_distribution<size_t> dist(0, n - 1);

			for (size_t i = 0; i < k; ++i) {
				centroids[i] = data[dist(rng)];
			}

			for (size_t it = 0; it < max_iter; ++it) {
				bool changed = false;
				std::vector<std::vector<float>> sums(k, std::vector<float>(dim, 0.0f));
				std::vector<size_t> counts(k, 0);

				// E-Step: Assign points
				for (size_t i = 0; i < n; ++i) {
					float min_dist = std::numeric_limits<float>::max();
					int best_c = 0;

					for (size_t c = 0; c < k; ++c) {
						// OPTIMIZATION: Use SimSIMD for F32 L2 distance
						simsimd_distance_t d;
						simsimd_l2sq_f32(data[i].data(), centroids[c].data(), static_cast<simsimd_size_t>(dim), &d);

						float dist_val = static_cast<float>(d);
						if (dist_val < min_dist) {
							min_dist = dist_val;
							best_c = c;
						}
					}
					if (assignment[i] != best_c)
						changed = true;
					assignment[i] = best_c;

					// Accumulate for M-Step
					for (size_t d = 0; d < dim; ++d)
						sums[best_c][d] += data[i][d];
					counts[best_c]++;
				}

				if (!changed)
					break;

				// M-Step: Update centroids
				for (size_t c = 0; c < k; ++c) {
					if (counts[c] > 0) {
						float inv_count = 1.0f / static_cast<float>(counts[c]);
						for (size_t d = 0; d < dim; ++d)
							centroids[c][d] = sums[c][d] * inv_count;
					} else {
						// Re-init empty cluster
						centroids[c] = data[dist(rng)];
					}
				}
			}
			return centroids;
		}
	};
} // namespace graph::vector
