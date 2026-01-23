/**
 * @file DiskANNIndex.hpp
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

#include "graph/vector/core/VectorMetric.hpp"
#include <unordered_set>

namespace graph::vector {
	class NativeProductQuantizer;
	class VectorIndexRegistry;

	struct DiskANNConfig {
		uint32_t dim;
		uint32_t beamWidth = 100;
		uint32_t maxDegree = 64;
		float alpha = 1.2f;
	};

	class DiskANNIndex {
	public:
		DiskANNIndex(std::shared_ptr<VectorIndexRegistry> registry, DiskANNConfig config);

		void insert(int64_t nodeId, const std::vector<float>& vec) const;
		std::vector<std::pair<int64_t, float>> search(const std::vector<float>& query, size_t k) const;

		// Train PQ model with sample data and save to registry
		void train(const std::vector<std::vector<float>>& samples);

	private:
		std::shared_ptr<VectorIndexRegistry> registry_;
		DiskANNConfig config_;
		std::unique_ptr<NativeProductQuantizer> quantizer_;

		std::vector<std::pair<int64_t, float>> greedySearch(
			const std::vector<float>& query,
			int64_t startNode,
			size_t beamWidth,
			const std::vector<float>& pqTable) const;

		void prune(int64_t nodeId, std::vector<int64_t>& candidates) const;

		float distRaw(const std::vector<float>& query, int64_t targetId) const;
		static float distRaw(const std::vector<float>& vecA, const std::vector<float>& vecB);
		float distPQ(const std::vector<float>& pqTable, int64_t targetId) const;
	};
}