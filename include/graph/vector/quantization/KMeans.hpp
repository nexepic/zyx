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

#include <cstddef>
#include <limits>
#include <random>
#include <vector>
#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/vector/core/VectorMetric.hpp"

namespace graph::vector {
	class KMeans {
	public:
		static std::vector<std::vector<float>> run(const std::vector<std::vector<float>> &data, size_t k,
												   size_t max_iter = 15,
												   concurrent::ThreadPool *pool = nullptr) {
			if (data.empty())
				return {};
			size_t dim = data[0].size();
			size_t n = data.size();

			// Initialize Centroids
			std::vector centroids(k, std::vector<float>(dim));
			std::vector<int> assignment(n);
			std::mt19937 rng(42);
			std::uniform_int_distribution<size_t> dist(0, n - 1);

			for (size_t i = 0; i < k; ++i) {
				centroids[i] = data[dist(rng)];
			}

			bool canParallelize = pool && !pool->isSingleThreaded() && n >= 256;

			for (size_t it = 0; it < max_iter; ++it) {
				if (canParallelize) {
					struct PartitionState {
						std::vector<std::vector<float>> sums;
						std::vector<size_t> counts;
						bool changed = false;
					};
					bool changedAny = false;
					std::vector sums(k, std::vector(dim, 0.0f));
					std::vector<size_t> counts(k, 0);

					(void) concurrent::ParallelOperatorExecutor::runRangePartitions<PartitionState>(
							0,
							n,
							pool,
							{.phase = "vector.kmeans.assignment",
							 .workloadKind = concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
							 .estimatedItems = n,
							 .minPartitions = 2,
							 .minItems = 256,
							 .minItemsPerWorker = 64},
							[&](const concurrent::ParallelRangePartition &range, PartitionState &state) {
								state.sums.assign(k, std::vector<float>(dim, 0.0f));
								state.counts.assign(k, 0);

								for (size_t i = range.begin; i < range.end; ++i) {
									float min_dist = std::numeric_limits<float>::max();
									int best_c = 0;

									for (size_t c = 0; c < k; ++c) {
										float dist_val = VectorMetric::computeL2Sqr(data[i].data(), centroids[c].data(), dim);
										if (dist_val < min_dist) {
											min_dist = dist_val;
											best_c = static_cast<int>(c);
										}
									}
									if (assignment[i] != best_c) {
										state.changed = true;
										assignment[i] = best_c;
									}

									for (size_t d = 0; d < dim; ++d) {
										state.sums[static_cast<size_t>(best_c)][d] += data[i][d];
									}
									state.counts[static_cast<size_t>(best_c)]++;
								}
							},
							[&](size_t, PartitionState &state) {
								changedAny = changedAny || state.changed;
								for (size_t c = 0; c < k; ++c) {
									counts[c] += state.counts[c];
									for (size_t d = 0; d < dim; ++d) {
										sums[c][d] += state.sums[c][d];
									}
								}
							});

					if (!changedAny) {
						break;
					}

					// M-Step: Update centroids
					for (size_t c = 0; c < k; ++c) {
						if (counts[c] > 0) {
							float inv_count = 1.0f / static_cast<float>(counts[c]);
							for (size_t d = 0; d < dim; ++d) {
								centroids[c][d] = sums[c][d] * inv_count;
							}
						} else {
							centroids[c] = data[dist(rng)];
						}
					}
				} else {
					// Sequential path (original)
					bool changedSeq = false;
					std::vector sums(k, std::vector(dim, 0.0f));
					std::vector<size_t> counts(k, 0);

					for (size_t i = 0; i < n; ++i) {
						float min_dist = std::numeric_limits<float>::max();
						int best_c = 0;

						for (size_t c = 0; c < k; ++c) {
							float dist_val = VectorMetric::computeL2Sqr(data[i].data(), centroids[c].data(), dim);
							if (dist_val < min_dist) {
								min_dist = dist_val;
								best_c = c;
							}
						}
						if (assignment[i] != best_c)
							changedSeq = true;
						assignment[i] = best_c;

						for (size_t d = 0; d < dim; ++d)
							sums[best_c][d] += data[i][d];
						counts[best_c]++;
					}

					if (!changedSeq)
						break;

					for (size_t c = 0; c < k; ++c) {
						if (counts[c] > 0) {
							float inv_count = 1.0f / static_cast<float>(counts[c]);
							for (size_t d = 0; d < dim; ++d)
								centroids[c][d] = sums[c][d] * inv_count;
						} else {
							centroids[c] = data[dist(rng)];
						}
					}
				}
			}
			return centroids;
		}
	};
} // namespace graph::vector
