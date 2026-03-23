/**
 * @file NativeProductQuantizer.hpp
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

#include <memory>
#include <vector>
#include "KMeans.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::vector {
	class NativeProductQuantizer {
	public:
		NativeProductQuantizer(size_t dim, size_t numSubspaces, size_t numCentroids = 256) :
			dim_(dim), numSubspaces_(numSubspaces), numCentroids_(numCentroids) {

			if (dim_ % numSubspaces_ != 0) {
				throw std::invalid_argument("Dimension must be divisible by number of subspaces");
			}
			subDim_ = dim_ / numSubspaces_;
		}

		void train(const std::vector<std::vector<float>> &trainingData,
				   concurrent::ThreadPool *pool = nullptr) {
			if (trainingData.empty())
				return;

			codebooks_.resize(numSubspaces_);

			// When parallelizing across subspaces, each KMeans runs sequentially
			// (passing pool to KMeans from inside parallelFor would cause deadlock).
			// When running subspaces sequentially, pass pool to KMeans for inner parallelism.
			auto trainSubspace = [&](size_t m, concurrent::ThreadPool *kmeansPool) {
				std::vector<std::vector<float>> subData;
				subData.reserve(trainingData.size());

				size_t offset = m * subDim_;

				// Integrity check
				if (offset + subDim_ > dim_) {
					throw std::runtime_error("PQ Subspace offset out of bounds");
				}

				for (const auto &vec : trainingData) {
					if (vec.size() != dim_) {
						throw std::runtime_error("Training vector dimension mismatch");
					}
					std::vector<float> sub(vec.begin() + offset, vec.begin() + offset + subDim_);
					subData.push_back(std::move(sub));
				}

				codebooks_[m] = KMeans::run(subData, numCentroids_, 15, kmeansPool);
			};

			if (pool && !pool->isSingleThreaded()) {
				// Outer parallelism across subspaces; KMeans runs sequentially per subspace
				pool->parallelFor(0, numSubspaces_, [&](size_t m) {
					trainSubspace(m, nullptr);
				});
			} else {
				// Sequential subspaces; each KMeans gets the pool for inner parallelism
				for (size_t m = 0; m < numSubspaces_; ++m)
					trainSubspace(m, pool);
			}
			isTrained_ = true;
		}

		[[nodiscard]] std::vector<uint8_t> encode(const std::vector<float> &vec,
											  concurrent::ThreadPool *pool = nullptr) const {
			if (!isTrained_)
				throw std::runtime_error("PQ not trained");
			if (vec.size() != dim_)
				throw std::runtime_error("Encode vector dimension mismatch");

			std::vector<uint8_t> codes(numSubspaces_);

			auto encodeSubspace = [&](size_t m) {
				size_t offset = m * subDim_;
				float min_dist = std::numeric_limits<float>::max();
				uint8_t best_idx = 0;

				const float *subVecPtr = vec.data() + offset;

				for (size_t c = 0; c < numCentroids_; ++c) {
					float dist = VectorMetric::computeL2Sqr(subVecPtr, codebooks_[m][c].data(), subDim_);

					if (dist < min_dist) {
						min_dist = dist;
						best_idx = static_cast<uint8_t>(c);
					}
				}
				codes[m] = best_idx;
			};

			// Only parallelize for high subspace counts; for <=32 subspaces
			// thread dispatch overhead exceeds the compute savings
			if (pool && !pool->isSingleThreaded() && numSubspaces_ > 32) {
				pool->parallelFor(0, numSubspaces_, encodeSubspace);
			} else {
				for (size_t m = 0; m < numSubspaces_; ++m)
					encodeSubspace(m);
			}
			return codes;
		}

		[[nodiscard]] std::vector<float> computeDistanceTable(const std::vector<float> &query,
													   concurrent::ThreadPool *pool = nullptr) const {
			std::vector<float> table(numSubspaces_ * numCentroids_);

			auto computeSubspace = [&](size_t m) {
				size_t offset = m * subDim_;
				const float *querySubPtr = query.data() + offset;

				for (size_t c = 0; c < numCentroids_; ++c) {
					float dist = VectorMetric::computeL2Sqr(querySubPtr, codebooks_[m][c].data(), subDim_);
					table[m * numCentroids_ + c] = dist;
				}
			};

			if (pool && !pool->isSingleThreaded() && numSubspaces_ > 32) {
				pool->parallelFor(0, numSubspaces_, computeSubspace);
			} else {
				for (size_t m = 0; m < numSubspaces_; ++m)
					computeSubspace(m);
			}
			return table;
		}

		static float computeDistance(const std::vector<uint8_t> &codes, const std::vector<float> &distTable,
									 size_t numSubspaces, size_t numCentroids = 256) {
			float dist = 0.0f;
			size_t m = 0;
			size_t stride = numCentroids;

			// Manual loop unrolling for pipelining
			for (; m + 3 < numSubspaces; m += 4) {
				dist += distTable[(m + 0) * stride + codes[m + 0]];
				dist += distTable[(m + 1) * stride + codes[m + 1]];
				dist += distTable[(m + 2) * stride + codes[m + 2]];
				dist += distTable[(m + 3) * stride + codes[m + 3]];
			}
			for (; m < numSubspaces; ++m) {
				dist += distTable[m * stride + codes[m]];
			}
			return dist;
		}

		// Serialization
		void serialize(std::ostream &os) const {
			utils::Serializer::writePOD(os, dim_);
			utils::Serializer::writePOD(os, numSubspaces_);
			utils::Serializer::writePOD(os, numCentroids_);
			utils::Serializer::writePOD(os, isTrained_);
			if (isTrained_) {
				for (const auto &subspace: codebooks_) {
					for (const auto &centroid: subspace) {
						for (float v: centroid)
							utils::Serializer::writePOD(os, v);
					}
				}
			}
		}

		static std::unique_ptr<NativeProductQuantizer> deserialize(std::istream &is) {
			size_t dim = utils::Serializer::readPOD<size_t>(is);
			size_t subs = utils::Serializer::readPOD<size_t>(is);
			size_t cents = utils::Serializer::readPOD<size_t>(is);
			bool trained = utils::Serializer::readPOD<bool>(is);

			auto pq = std::make_unique<NativeProductQuantizer>(dim, subs, cents);
			pq->isTrained_ = trained;

			if (trained) {
				pq->codebooks_.resize(subs);
				size_t subDim = dim / subs;
				for (size_t m = 0; m < subs; ++m) {
					pq->codebooks_[m].resize(cents);
					for (size_t c = 0; c < cents; ++c) {
						pq->codebooks_[m][c].resize(subDim);
						for (size_t d = 0; d < subDim; ++d)
							pq->codebooks_[m][c][d] = utils::Serializer::readPOD<float>(is);
					}
				}
			}
			return pq;
		}

		[[nodiscard]] bool isTrained() const { return isTrained_; }

	private:
		size_t dim_;
		size_t numSubspaces_;
		size_t subDim_;
		size_t numCentroids_;
		bool isTrained_ = false;
		std::vector<std::vector<std::vector<float>>> codebooks_;
	};
} // namespace graph::vector
