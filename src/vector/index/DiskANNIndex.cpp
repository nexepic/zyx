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
#include "graph/log/Log.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/core/BFloat16.hpp"

namespace graph::vector {

	DiskANNIndex::DiskANNIndex(std::shared_ptr<VectorIndexRegistry> registry, const DiskANNConfig &config) :
		registry_(std::move(registry)), config_(config) {
		// Load quantizer from storage on init
		quantizer_ = registry_->loadQuantizer();
	}

	bool DiskANNIndex::isPQTrained() const {
        return quantizer_ && quantizer_->isTrained();
    }

    size_t DiskANNIndex::size() const {
        return cachedCount_;
    }

    void DiskANNIndex::insert(int64_t nodeId, const std::vector<float> &vec) {
        // --- 1. Auto-Training Logic ---
        // If not trained and threshold reached, trigger training synchronously.
        if (!isPQTrained()) {
            cachedCount_++; // Increment count
            if (cachedCount_ == config_.autoTrainThreshold) {
                log::Log::info("Vector Index Auto-Training Triggered (Count: {})", cachedCount_);
                auto samples = sampleVectors(config_.autoTrainThreshold);
                // Add current vector to samples to ensure sufficiency
                samples.push_back(vec);
                train(samples);
            }
        }

        // --- 2. Save Data ---
        // Always save Raw Vector (Ground Truth)
        const auto rawBlob = registry_->saveRawVector(toBFloat16(vec));

        // Conditionally save PQ Codes
        int64_t pqBlob = 0;
        if (isPQTrained()) {
            pqBlob = registry_->savePQCodes(quantizer_->encode(vec));
        }

        // Update Registry Mapping
        VectorBlobPtrs ptrs;
        ptrs.rawBlob = rawBlob;
        ptrs.pqBlob = pqBlob;
        ptrs.adjBlob = 0;
        registry_->setBlobPtrs(nodeId, ptrs);

        const int64_t entryPoint = registry_->getConfig().entryPointNodeId;

        // --- 3. Graph Construction ---

        // Case A: First Node
        if (entryPoint == 0) {
            registry_->updateEntryPoint(nodeId);
            ptrs.adjBlob = registry_->saveAdjacency({});
            registry_->setBlobPtrs(nodeId, ptrs);
            return;
        }

        // Case B: Connect to Graph
        std::vector<float> pqTable;
        if (isPQTrained()) {
            pqTable = quantizer_->computeDistanceTable(vec);
        }

        // Search for nearest neighbors
        auto searchRes = greedySearch(vec, entryPoint, config_.beamWidth, pqTable);

        std::vector<int64_t> candidates;
        candidates.reserve(searchRes.size());
        for (auto &key: searchRes | std::views::keys)
            candidates.push_back(key);

        // Prune and link
        prune(nodeId, candidates);

        // Save Adjacency
        ptrs.adjBlob = registry_->saveAdjacency(candidates);
        registry_->setBlobPtrs(nodeId, ptrs);

        // Add Back-Links
        for (int64_t neighbor: candidates) {
            auto nPtrs = registry_->getBlobPtrs(neighbor);
            if (nPtrs.adjBlob == 0) continue;

            auto nList = registry_->loadAdjacency(nPtrs.adjBlob);
            bool exists = false;
            for(auto id : nList) if(id == nodeId) { exists = true; break; }

            if (!exists) {
                nList.push_back(nodeId);
                // Robust Prune again if overflow
                if (nList.size() > config_.maxDegree * 1.2) {
                    prune(neighbor, nList);
                }
                nPtrs.adjBlob = registry_->saveAdjacency(nList);
                registry_->setBlobPtrs(neighbor, nPtrs);
            }
        }
    }

    std::vector<std::pair<int64_t, float>> DiskANNIndex::search(const std::vector<float> &query, size_t k) const {
        int64_t entryPoint = registry_->getConfig().entryPointNodeId;
        if (entryPoint == 0) return {};

        std::vector<float> pqTable;
        if (isPQTrained()) {
            pqTable = quantizer_->computeDistanceTable(query);
        }

        // 1. Navigation (Hybrid Mode)
        auto candidates = greedySearch(query, entryPoint, std::max(config_.beamWidth, static_cast<uint32_t>(k) * 2), pqTable);

        // 2. Re-ranking (Always Exact)
		std::vector<std::pair<int64_t, float>> refined;
		refined.reserve(candidates.size());

		for (auto &id: candidates | std::views::keys) {
			float d = distRaw(query, id);
			// Filter out removed nodes (Infinite distance)
			if (d < std::numeric_limits<float>::max()) {
				refined.push_back({id, d});
			}
		}

		std::ranges::sort(refined, [](const auto &a, const auto &b) {
			return a.second < b.second;
		});

		if (refined.size() > k) refined.resize(k);
		return refined;
    }

    // --- Sampling & Training ---

    std::vector<std::vector<float>> DiskANNIndex::sampleVectors(size_t n) const {
        // Delegate sampling to Registry to avoid leaking B-Tree logic here.
        // But since we don't want to overcomplicate Registry yet, we can iterate crudely here
        // or add a method to Registry. Let's add `getAllNodeIds` to Registry.

        std::vector<std::vector<float>> samples;
        auto ids = registry_->getAllNodeIds(n * 2); // Get 2x candidates to be safe, or just N

        // Reservoir sampling or just take first N
        size_t count = 0;
        for (int64_t id : ids) {
            if (count >= n) break;

            auto ptrs = registry_->getBlobPtrs(id);
            if (ptrs.rawBlob != 0) {
                auto raw = registry_->loadRawVector(ptrs.rawBlob);
                samples.push_back(toFloat(raw));
                count++;
            }
        }
        return samples;
    }

    void DiskANNIndex::train(const std::vector<std::vector<float>>& samples) {
        if (samples.empty()) return;

        // Heuristic: M (subspaces) = dim / 8 (e.g. 768 -> 96 subspaces)
        // Ensure sub-dim is small enough for efficient lookup tables
        size_t subDim = 8;
        size_t m = config_.dim / subDim;
        if (m == 0) m = 1;

        log::Log::info("Training PQ Model: Dim={}, Subspaces={}, Samples={}", config_.dim, m, samples.size());

        auto pq = std::make_unique<NativeProductQuantizer>(config_.dim, m);
        pq->train(samples);

        registry_->saveQuantizer(*pq);
        quantizer_ = std::move(pq);

        // Note: We do NOT update existing nodes here.
        // Old nodes remain "Flat" (Raw-only). This is the "Hybrid" design.
        // Only new nodes will get PQ blobs.
    }

    // --- Unified Distance ---

    float DiskANNIndex::computeDistance(const std::vector<float>& query,
                                        const std::vector<float>& pqTable,
                                        int64_t targetId) const {
        // Hybrid Logic: Use PQ if available, else Raw
        if (isPQTrained() && !pqTable.empty()) {
            auto ptrs = registry_->getBlobPtrs(targetId);
            if (ptrs.pqBlob != 0) {
                return distPQ(pqTable, targetId);
            }
        }
        return distRaw(query, targetId);
    }

    float DiskANNIndex::computeDistance(const std::vector<float>& nodeVec, int64_t targetId) const {
        // Overload for Pruning: Distance between Node A (in memory) and Node B (in storage)
        // Currently we use Raw for pruning accuracy.
        // Optimization: Could use PQ here too if pruning speed is bottleneck.
        return distRaw(nodeVec, targetId);
    }

    // --- Graph Algo ---

    std::vector<std::pair<int64_t, float>> DiskANNIndex::greedySearch(
        const std::vector<float> &query,
        int64_t startNode,
        size_t beamWidth,
        const std::vector<float> &pqTable) const {

        std::unordered_set<int64_t> visited;
        std::priority_queue<std::pair<float, int64_t>, std::vector<std::pair<float, int64_t>>, std::greater<>> queue;
        std::vector<std::pair<int64_t, float>> results;

        float startDist = computeDistance(query, pqTable, startNode);
        queue.push({startDist, startNode});
        visited.insert(startNode);
        results.push_back({startNode, startDist});

        while (!queue.empty()) {
            auto [d, u] = queue.top();
            queue.pop();

            auto ptrs = registry_->getBlobPtrs(u);
            if (ptrs.adjBlob == 0) continue;

            auto neighbors = registry_->loadAdjacency(ptrs.adjBlob);
            for (int64_t v: neighbors) {
                if (visited.contains(v)) continue;
                visited.insert(v);

                float dist = computeDistance(query, pqTable, v);

                results.push_back({v, dist});
                queue.push({dist, v});
            }
        }

        std::ranges::sort(results, [](const auto &a, const auto &b) { return a.second < b.second; });
        if (results.size() > beamWidth) results.resize(beamWidth);
        return results;
    }

    void DiskANNIndex::prune(int64_t nodeId, std::vector<int64_t> &candidates) const {
        // Load target node RAW vector for accurate pruning geometry
        const auto ptrs = registry_->getBlobPtrs(nodeId);
        if (ptrs.rawBlob == 0) return;

        // This vector might be large, but needed for Alpha calculation
        std::vector<float> nodeVec = toFloat(registry_->loadRawVector(ptrs.rawBlob));

        // 1. Sort by distance
        std::vector<std::pair<int64_t, float>> candDists;
        for (int64_t c: candidates) {
            if (c == nodeId) continue;
            // Use specialized overload for (Vec vs ID)
            candDists.push_back({c, computeDistance(nodeVec, c)});
        }
        std::ranges::sort(candDists, [](const auto &a, const auto &b) { return a.second < b.second; });

        // 2. Alpha Pruning
        std::vector<int64_t> result;
        result.reserve(config_.maxDegree);

        for (const auto &[candId, candDist]: candDists) {
            if (result.size() >= config_.maxDegree) break;

            bool occluded = false;
            // We need distance between candidate (ID) and existing result (ID)
            // Here we must load one of them. Loading Candidate is cheaper in loop?
            // Actually, we can load Candidate Raw Vector once.

            auto candPtrs = registry_->getBlobPtrs(candId);
            std::vector<float> candVec = toFloat(registry_->loadRawVector(candPtrs.rawBlob));

            for (int64_t existingId: result) {
                // Here we calculate distance between two in-memory vectors
                // Overload needed or manual calc
                float distP2C = distRaw(candVec, toFloat(registry_->loadRawVector(registry_->getBlobPtrs(existingId).rawBlob)));
                // Optimization: Cache existingId vectors in 'result' loop?
                // For embedded, reading Blob Cache is fast.

                if (config_.alpha * distP2C <= candDist) {
                    occluded = true;
                    break;
                }
            }
            if (!occluded) result.push_back(candId);
        }
        candidates = result;
    }

	// --- Distance Helpers ---

	float DiskANNIndex::distRaw(const std::vector<float> &query, int64_t targetId) const {
		const auto ptrs = registry_->getBlobPtrs(targetId);
		if (ptrs.rawBlob == 0) return std::numeric_limits<float>::max();
		auto target = registry_->loadRawVector(ptrs.rawBlob);

		if (config_.metric == "IP" || config_.metric == "Cosine") {
			return VectorMetric::computeIP(toBFloat16(query).data(), target.data(), target.size());
		}
		// Default L2
		return VectorMetric::computeL2Sqr(query.data(), target.data(), target.size());
	}

	float DiskANNIndex::distRaw(const std::vector<float> &vecA, const std::vector<float> &vecB) {
		float d = 0;
		for (size_t i = 0; i < vecA.size(); ++i) {
			float diff = vecA[i] - vecB[i];
			d += diff * diff;
		}
		return d;
	}

	float DiskANNIndex::distPQ(const std::vector<float> &pqTable, int64_t targetId) const {
		auto ptrs = registry_->getBlobPtrs(targetId);
		// computeDistance checks for 0, but safety here:
		if (ptrs.pqBlob == 0) return std::numeric_limits<float>::max();
		const auto codes = registry_->loadPQCodes(ptrs.pqBlob);
		const size_t M = codes.size();
		const size_t K = pqTable.size() / M;
		return NativeProductQuantizer::computeDistance(codes, pqTable, M, K);
	}

	void DiskANNIndex::remove(int64_t nodeId) {
		// Logical deletion: Remove from Registry Mapping
		// This effectively makes getBlobPtrs(nodeId) return {0,0,0}
		registry_->setBlobPtrs(nodeId, {0,0,0});
	}

} // namespace graph::vector
