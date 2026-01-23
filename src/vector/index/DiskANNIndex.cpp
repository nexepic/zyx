/**
 * @file DiskANNIndex.cpp
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

#include "graph/vector/index/DiskANNIndex.hpp"
#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/core/BFloat16.hpp"

namespace graph::vector {

	DiskANNIndex::DiskANNIndex(std::shared_ptr<VectorIndexRegistry> registry, DiskANNConfig config) :
		registry_(std::move(registry)), config_(config) {
		// Load quantizer from storage on init
		quantizer_ = registry_->loadQuantizer();
	}

	void DiskANNIndex::train(const std::vector<std::vector<float>> &samples) {
		if (samples.empty())
			return;

		// Auto-configure subspaces if not set
		// Standard heuristic: 1 subspace per 8-16 dimensions
		size_t subDim = 12;
		size_t M = config_.dim / subDim;
		if (M == 0)
			M = 1; // Fallback for low dim

		quantizer_ = std::make_unique<NativeProductQuantizer>(config_.dim, M);
		quantizer_->train(samples);

		// Persist trained model
		registry_->saveQuantizer(*quantizer_);
	}

	void DiskANNIndex::insert(int64_t nodeId, const std::vector<float> &vec) const {
		if (!quantizer_ || !quantizer_->isTrained()) {
			throw std::runtime_error("Index not trained. Call train() first.");
		}

		// 1. Prepare Data Blobs
		const auto rawBlob = registry_->saveRawVector(toBFloat16(vec));
		const auto pqBlob = registry_->savePQCodes(quantizer_->encode(vec));

		// Update Mapping immediately so distRaw works during pruning
		VectorBlobPtrs ptrs;
		ptrs.rawBlob = rawBlob;
		ptrs.pqBlob = pqBlob;
		ptrs.adjBlob = 0; // Empty initially
		registry_->setBlobPtrs(nodeId, ptrs);

		const int64_t entryPoint = registry_->getConfig().entryPointNodeId;

		// 2. Init Graph (First Node)
		if (entryPoint == 0) {
			registry_->updateEntryPoint(nodeId);
			// Save empty adjacency list
			int64_t adjBlob = registry_->saveAdjacency({});
			ptrs.adjBlob = adjBlob;
			registry_->setBlobPtrs(nodeId, ptrs);
			return;
		}

		// 3. Greedy Search to find candidates
		std::vector<float> pqTable = quantizer_->computeDistanceTable(vec);
		auto searchRes = greedySearch(vec, entryPoint, config_.beamWidth, pqTable);

		std::vector<int64_t> candidates;
		candidates.reserve(searchRes.size());
		for (auto &key: searchRes | std::views::keys)
			candidates.push_back(key);

		// 4. Prune & Save Forward Edges
		prune(nodeId, candidates);
		int64_t adjBlob = registry_->saveAdjacency(candidates);

		// Final Mapping Update
		ptrs.adjBlob = adjBlob;
		registry_->setBlobPtrs(nodeId, ptrs);

		// 5. Add Back-Links (Bidirectional connection)
		for (int64_t neighbor: candidates) {
			auto nPtrs = registry_->getBlobPtrs(neighbor);
			// In a valid graph, nPtrs.adjBlob should not be 0, but check for safety
			if (nPtrs.adjBlob == 0)
				continue;

			auto nList = registry_->loadAdjacency(nPtrs.adjBlob);

			// Add self to neighbor's list
			// Check duplicates first to be safe
			bool exists = false;
			for (auto id: nList)
				if (id == nodeId) {
					exists = true;
					break;
				}

			if (!exists) {
				nList.push_back(nodeId);

				// Only prune if exceeding capacity to minimize IO
				if (nList.size() > config_.maxDegree * 1.2) { // Allow slight overflow before prune
					prune(neighbor, nList);
				}

				int64_t newAdj = registry_->saveAdjacency(nList);
				nPtrs.adjBlob = newAdj;
				registry_->setBlobPtrs(neighbor, nPtrs);
			}
		}
	}

	std::vector<std::pair<int64_t, float>> DiskANNIndex::search(const std::vector<float> &query, size_t k) const {
		int64_t entryPoint = registry_->getConfig().entryPointNodeId;
		if (entryPoint == 0)
			return {};

		// 1. Coarse Search (PQ-Accelerated)
		std::vector<float> pqTable;
		if (quantizer_) {
			pqTable = quantizer_->computeDistanceTable(query);
		}

		// Search with larger beam width for recall
		auto candidates =
				greedySearch(query, entryPoint, std::max(config_.beamWidth, static_cast<uint32_t>(k) * 2), pqTable);

		// 2. Re-rank (Exact BFloat16 via SimSIMD)
		std::vector<std::pair<int64_t, float>> refined;
		refined.reserve(candidates.size());

		for (auto &id: candidates | std::views::keys) {
			refined.push_back({id, distRaw(query, id)});
		}

		std::ranges::sort(refined, [](const auto &a, const auto &b) {
			return a.second < b.second; // Ascending distance
		});

		if (refined.size() > k)
			refined.resize(k);
		return refined;
	}

	std::vector<std::pair<int64_t, float>> DiskANNIndex::greedySearch(const std::vector<float> &query,
																	  int64_t startNode, size_t beamWidth,
																	  const std::vector<float> &pqTable) const {
		std::unordered_set<int64_t> visited;
		// Min-heap <dist, id> to keep top candidates
		std::priority_queue<std::pair<float, int64_t>, std::vector<std::pair<float, int64_t>>, std::greater<>> queue;
		std::vector<std::pair<int64_t, float>> results;

		// Init distance
		float startDist = (!pqTable.empty()) ? distPQ(pqTable, startNode) : distRaw(query, startNode);

		queue.push({startDist, startNode});
		visited.insert(startNode);
		results.push_back({startNode, startDist});

		while (!queue.empty()) {
			auto [d, u] = queue.top();
			queue.pop();

			// Load adjacency list
			auto ptrs = registry_->getBlobPtrs(u);
			if (ptrs.adjBlob == 0)
				continue;

			auto neighbors = registry_->loadAdjacency(ptrs.adjBlob);

			for (int64_t v: neighbors) {
				if (visited.contains(v))
					continue;
				visited.insert(v);

				float dist = (!pqTable.empty()) ? distPQ(pqTable, v) : distRaw(query, v);

				results.push_back({v, dist});
				queue.push({dist, v});
			}

			// Limit Queue size to avoid explosion (optional implementation detail)
		}

		// Sort and Top-K
		std::ranges::sort(results, [](const auto &a, const auto &b) { return a.second < b.second; });

		if (results.size() > beamWidth) {
			results.resize(beamWidth);
		}

		return results;
	}

	void DiskANNIndex::prune(int64_t nodeId, std::vector<int64_t> &candidates) const {
		// Robust Prune (Vamana Algo 2)

		const auto ptrs = registry_->getBlobPtrs(nodeId);
		const auto nodeVecRaw = registry_->loadRawVector(ptrs.rawBlob);
		if (nodeVecRaw.empty())
			return;

		// Convert BFloat16 back to float for calculation
		// Fix: using explicit namespace or argument dependent lookup
		std::vector<float> nodeVec = toFloat(nodeVecRaw);

		// 1. Calculate distances to all candidates
		std::vector<std::pair<int64_t, float>> candDists;
		for (int64_t c: candidates) {
			if (c == nodeId)
				continue;
			// Use the float-vector overload of distRaw
			candDists.push_back({c, distRaw(nodeVec, c)});
		}

		// 2. Sort by distance
		std::ranges::sort(candDists, [](const auto &a, const auto &b) { return a.second < b.second; });

		std::vector<int64_t> result;
		result.reserve(config_.maxDegree);

		// 3. Alpha Pruning
		for (const auto &[candId, candDist]: candDists) {
			if (result.size() >= config_.maxDegree)
				break;

			bool occluded = false;

			auto candPtrs = registry_->getBlobPtrs(candId);
			auto candVecRaw = registry_->loadRawVector(candPtrs.rawBlob);
			std::vector<float> candVec = toFloat(candVecRaw);

			for (int64_t existingId: result) {
				// distRaw overload for two vectors
				float distP2C = distRaw(candVec, existingId);

				if (config_.alpha * distP2C <= candDist) {
					occluded = true;
					break;
				}
			}

			if (!occluded) {
				result.push_back(candId);
			}
		}
		candidates = result;
	}

	// --- Distance Helpers ---

	float DiskANNIndex::distRaw(const std::vector<float> &query, int64_t targetId) const {
		// Load BF16 from Blob storage
		const auto ptrs = registry_->getBlobPtrs(targetId);
		if (ptrs.rawBlob == 0)
			return std::numeric_limits<float>::max();

		auto target = registry_->loadRawVector(ptrs.rawBlob);
		return VectorMetric::computeL2Sqr(query.data(), target.data(), target.size());
	}

	// This helper was missing/conflicted.
	// It loads the targetId's vector and compares it against vecA (which is in memory)
	float DiskANNIndex::distRaw(const std::vector<float> &vecA, const std::vector<float> &vecB) {
		// Manual L2 for pruning (float vs float)
		float d = 0;
		for (size_t i = 0; i < vecA.size(); ++i) {
			float diff = vecA[i] - vecB[i];
			d += diff * diff;
		}
		return d;
	}

	float DiskANNIndex::distPQ(const std::vector<float> &pqTable, int64_t targetId) const {
		auto ptrs = registry_->getBlobPtrs(targetId);
		if (ptrs.pqBlob == 0)
			return std::numeric_limits<float>::max();

		const auto codes = registry_->loadPQCodes(ptrs.pqBlob);
		if (codes.empty())
			return std::numeric_limits<float>::max();

		const size_t M = codes.size();
		const size_t K = pqTable.size() / M;
		return NativeProductQuantizer::computeDistance(codes, pqTable, M, K);
	}

} // namespace graph::vector
