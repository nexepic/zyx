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
#include <functional>
#include <limits>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/log/Log.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/core/BFloat16.hpp"
#include "graph/vector/core/VectorMetric.hpp"

namespace graph::vector {

	DiskANNIndex::DiskANNIndex(std::shared_ptr<VectorIndexRegistry> registry, const DiskANNConfig &config) :
		registry_(std::move(registry)), config_(config) {
		// Load quantizer from storage on init
		quantizer_ = registry_->loadQuantizer();
	}

	bool DiskANNIndex::isPQTrained() const { return quantizer_ && quantizer_->isTrained(); }

	size_t DiskANNIndex::size() const { return cachedCount_; }

	void DiskANNIndex::insert(int64_t nodeId, const std::vector<float> &vec) {
		if (vec.size() != config_.dim) {
			log::Log::error("Insert skipped: Vector dimension mismatch. Expected {}, got {}", config_.dim, vec.size());
			return; // Skip insertion
		}

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
		const auto rawBlob = registry_->saveRawVector(toBFloat16(vec));

		log::Log::debug("[DEBUG] Saved RawBlob ID: {} for Node {}", rawBlob, nodeId);

		// Conditionally save PQ Codes
		int64_t pqBlob = 0;
		if (isPQTrained())
			pqBlob = registry_->savePQCodes(quantizer_->encode(vec));

		VectorBlobPtrs ptrs;
		ptrs.rawBlob = rawBlob;
		ptrs.pqBlob = pqBlob;

		const int64_t entryPoint = registry_->getConfig().entryPointNodeId;

		log::Log::debug("Insert Node {}. EntryPoint: {}", nodeId, entryPoint);

		// --- 3. Graph Construction ---

		// Case A: First Node
		if (entryPoint == 0) {
			registry_->updateEntryPoint(nodeId);
			ptrs.adjBlob = registry_->saveAdjacency({});
			registry_->setBlobPtrs(nodeId, ptrs);
			return;
		}

		// Case B: Connect to Graph
		// Save initial mapping so greedy search can find this node
		ptrs.adjBlob = 0;
		registry_->setBlobPtrs(nodeId, ptrs);

		std::vector<float> pqTable;
		if (isPQTrained())
			pqTable = quantizer_->computeDistanceTable(vec);

		auto searchRes = greedySearch(vec, entryPoint, config_.beamWidth, pqTable);

		std::vector<int64_t> candidates;
		candidates.reserve(searchRes.size());
		for (auto &key: searchRes | std::views::keys)
			candidates.push_back(key);

		prune(nodeId, candidates);

		ptrs.adjBlob = registry_->saveAdjacency(candidates);
		registry_->setBlobPtrs(nodeId, ptrs);

		// Add Back-Links
		for (int64_t neighbor: candidates) {
			auto nPtrs = registry_->getBlobPtrs(neighbor);
			if (nPtrs.adjBlob == 0)
				continue;

			auto nList = registry_->loadAdjacency(nPtrs.adjBlob);
			bool exists = false;
			for (auto id: nList)
				if (id == nodeId) {
					exists = true;
					break;
				}

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

	void DiskANNIndex::batchInsert(const std::vector<std::pair<int64_t, std::vector<float>>> &batch) {
		if (batch.empty())
			return;

		// Phase 1: Auto-training check with entire batch
		if (!isPQTrained()) {
			cachedCount_ += batch.size();
			if (cachedCount_ >= config_.autoTrainThreshold) {
				log::Log::info("Vector Index Auto-Training Triggered (Count: {})", cachedCount_);
				auto samples = sampleVectors(config_.autoTrainThreshold);
				for (const auto &[id, vec]: batch)
					samples.push_back(vec);
				train(samples);
			}
		}

		// Phase 2: Batch save all raw vectors and PQ codes
		struct InsertEntry {
			int64_t nodeId;
			const std::vector<float> *vec; // Pointer to original vector (avoids copy)
			VectorBlobPtrs ptrs{};
		};
		std::vector<InsertEntry> entries;
		entries.reserve(batch.size());

		for (const auto &[nodeId, vec]: batch) {
			if (vec.size() != config_.dim) {
				log::Log::error("Batch insert skipped node {}: dimension mismatch", nodeId);
				continue;
			}

			InsertEntry entry;
			entry.nodeId = nodeId;
			entry.vec = &vec;
			entry.ptrs.rawBlob = registry_->saveRawVector(toBFloat16(vec));
			entry.ptrs.pqBlob = isPQTrained() ? registry_->savePQCodes(quantizer_->encode(vec)) : 0;
			entry.ptrs.adjBlob = 0;
			registry_->setBlobPtrs(nodeId, entry.ptrs);
			entries.push_back(entry);
		}

		if (entries.empty())
			return;

		// Phase 3: Graph construction
		int64_t entryPoint = registry_->getConfig().entryPointNodeId;

		size_t startIdx = 0;
		if (entryPoint == 0) {
			auto &first = entries[0];
			registry_->updateEntryPoint(first.nodeId);
			first.ptrs.adjBlob = registry_->saveAdjacency({});
			registry_->setBlobPtrs(first.nodeId, first.ptrs);
			entryPoint = first.nodeId;
			startIdx = 1;
		}

		// Collect back-link updates to batch them
		std::unordered_map<int64_t, std::vector<int64_t>> pendingBackLinks;

		for (size_t idx = startIdx; idx < entries.size(); ++idx) {
			auto &entry = entries[idx];
			const auto &vec = *entry.vec;

			std::vector<float> pqTable;
			if (isPQTrained())
				pqTable = quantizer_->computeDistanceTable(vec);

			auto searchRes = greedySearch(vec, entryPoint, config_.beamWidth, pqTable);

			std::vector<int64_t> candidates;
			candidates.reserve(searchRes.size());
			for (auto &key: searchRes | std::views::keys)
				candidates.push_back(key);

			prune(entry.nodeId, candidates);

			entry.ptrs.adjBlob = registry_->saveAdjacency(candidates);
			registry_->setBlobPtrs(entry.nodeId, entry.ptrs);

			for (int64_t neighbor: candidates)
				pendingBackLinks[neighbor].push_back(entry.nodeId);
		}

		// Phase 4: Batch apply back-links (single I/O per neighbor)
		for (auto &[neighborId, newLinks]: pendingBackLinks) {
			auto nPtrs = registry_->getBlobPtrs(neighborId);
			if (nPtrs.adjBlob == 0)
				continue;

			auto nList = registry_->loadAdjacency(nPtrs.adjBlob);
			std::unordered_set<int64_t> existing(nList.begin(), nList.end());

			for (int64_t newId: newLinks) {
				if (!existing.contains(newId)) {
					nList.push_back(newId);
					existing.insert(newId);
				}
			}

			if (nList.size() > static_cast<size_t>(config_.maxDegree * 1.2))
				prune(neighborId, nList);

			nPtrs.adjBlob = registry_->saveAdjacency(nList);
			registry_->setBlobPtrs(neighborId, nPtrs);
		}
	}

	std::vector<std::pair<int64_t, float>> DiskANNIndex::search(const std::vector<float> &query, size_t k) const {
		int64_t entryPoint = registry_->getConfig().entryPointNodeId;
		if (entryPoint == 0)
			return {};

		std::vector<float> pqTable;
		if (isPQTrained()) {
			pqTable = quantizer_->computeDistanceTable(query, threadPool_);
		}

		// 1. Navigation (Hybrid Mode)
		auto candidates =
				greedySearch(query, entryPoint, std::max(config_.beamWidth, static_cast<uint32_t>(k) * 2), pqTable);

		// 2. Re-ranking (Always Exact) - parallelized
		// Collect candidate IDs for parallel distance computation
		std::vector<int64_t> candidateIds;
		candidateIds.reserve(candidates.size());
		for (auto &id : candidates | std::views::keys)
			candidateIds.push_back(id);

		std::vector<std::pair<int64_t, float>> refined(candidateIds.size());

		auto computeRank = [&](size_t i) {
			float d = distRaw(query, candidateIds[i]);
			refined[i] = {candidateIds[i], d};
		};

		if (threadPool_ && !threadPool_->isSingleThreaded() && candidateIds.size() > 16) {
			threadPool_->parallelFor(0, candidateIds.size(), computeRank);
		} else {
			for (size_t i = 0; i < candidateIds.size(); ++i)
				computeRank(i);
		}

		// Filter removed nodes and sort
		std::vector<std::pair<int64_t, float>> filtered;
		filtered.reserve(refined.size());
		for (auto &[id, d] : refined) {
			if (d < std::numeric_limits<float>::max())
				filtered.push_back({id, d});
		}

		std::ranges::sort(filtered, [](const auto &a, const auto &b) { return a.second < b.second; });

		if (filtered.size() > k)
			filtered.resize(k);
		return filtered;
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
		for (int64_t id: ids) {
			if (count >= n)
				break;

			auto ptrs = registry_->getBlobPtrs(id);
			if (ptrs.rawBlob != 0) {
				auto raw = registry_->loadRawVector(ptrs.rawBlob);
				samples.push_back(toFloat(raw));
				count++;
			}
		}
		return samples;
	}

	void DiskANNIndex::train(const std::vector<std::vector<float>> &samples) {
		if (samples.empty())
			return;

		// Heuristic: M (subspaces) = dim / 8 (e.g. 768 -> 96 subspaces)
		// Ensure sub-dim is small enough for efficient lookup tables
		size_t subDim = 8;
		size_t m = config_.dim / subDim;
		if (m == 0)
			m = 1;

		log::Log::info("Training PQ Model: Dim={}, Subspaces={}, Samples={}", config_.dim, m, samples.size());

		auto pq = std::make_unique<NativeProductQuantizer>(config_.dim, m);
		pq->train(samples, threadPool_);

		registry_->saveQuantizer(*pq);
		quantizer_ = std::move(pq);

		// Note: We do NOT update existing nodes here.
		// Old nodes remain "Flat" (Raw-only). This is the "Hybrid" design.
		// Only new nodes will get PQ blobs.
	}

	// --- Unified Distance ---

	float DiskANNIndex::computeDistance(const std::vector<float> &query, const std::vector<float> &pqTable,
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

	float DiskANNIndex::computeDistance(const std::vector<float> &nodeVec, int64_t targetId) const {
		// Overload for Pruning: Distance between Node A (in memory) and Node B (in storage)
		// Currently we use Raw for pruning accuracy.
		// Optimization: Could use PQ here too if pruning speed is bottleneck.
		return distRaw(nodeVec, targetId);
	}

	// --- Graph Algo ---

	std::vector<std::pair<int64_t, float>> DiskANNIndex::greedySearch(const std::vector<float> &query,
																	  int64_t startNode, size_t beamWidth,
																	  const std::vector<float> &pqTable) const {

		std::unordered_set<int64_t> visited;
		visited.reserve(beamWidth * 4);

		// Min-heap for expansion frontier (closest first)
		std::priority_queue<std::pair<float, int64_t>, std::vector<std::pair<float, int64_t>>, std::greater<>> frontier;
		// Max-heap for result set (furthest first, for bounded eviction)
		std::priority_queue<std::pair<float, int64_t>> results;

		float startDist = computeDistance(query, pqTable, startNode);
		frontier.push({startDist, startNode});
		results.push({startDist, startNode});
		visited.insert(startNode);

		while (!frontier.empty()) {
			auto [d, u] = frontier.top();
			frontier.pop();

			// Early termination: if closest unexpanded node is worse than
			// the furthest node in our result set, we can't improve
			if (results.size() >= beamWidth && d > results.top().first)
				break;

			auto ptrs = registry_->getBlobPtrs(u);
			if (ptrs.adjBlob == 0)
				continue;

			auto neighbors = registry_->loadAdjacency(ptrs.adjBlob);
			for (int64_t v: neighbors) {
				if (visited.contains(v))
					continue;
				visited.insert(v);

				float dist = computeDistance(query, pqTable, v);

				// Only add to frontier/results if it could improve our result set
				if (results.size() < beamWidth || dist < results.top().first) {
					frontier.push({dist, v});
					results.push({dist, v});
					if (results.size() > beamWidth)
						results.pop();
				}
			}
		}

		// Extract results from max-heap into sorted vector
		std::vector<std::pair<int64_t, float>> output;
		output.reserve(results.size());
		while (!results.empty()) {
			auto [dist, id] = results.top();
			results.pop();
			output.push_back({id, dist});
		}
		std::ranges::sort(output, [](const auto &a, const auto &b) { return a.second < b.second; });
		return output;
	}

	void DiskANNIndex::prune(int64_t nodeId, std::vector<int64_t> &candidates) const {
		const auto ptrs = registry_->getBlobPtrs(nodeId);
		if (ptrs.rawBlob == 0)
			return;

		std::vector<float> nodeVec = toFloat(registry_->loadRawVector(ptrs.rawBlob));

		// 1. Compute distances and sort candidates
		std::vector<std::pair<int64_t, float>> candDists;
		candDists.reserve(candidates.size());
		for (int64_t c: candidates) {
			if (c == nodeId)
				continue;
			candDists.push_back({c, computeDistance(nodeVec, c)});
		}
		std::ranges::sort(candDists, [](const auto &a, const auto &b) { return a.second < b.second; });

		// 2. Alpha pruning with vector cache to avoid redundant I/O
		std::vector<int64_t> result;
		std::vector<std::vector<float>> resultVecs; // Cache vectors of accepted nodes
		result.reserve(config_.maxDegree);
		resultVecs.reserve(config_.maxDegree);

		for (const auto &[candId, candDist]: candDists) {
			if (result.size() >= config_.maxDegree)
				break;

			// Load candidate vector once for all occlusion checks
			auto candPtrs = registry_->getBlobPtrs(candId);
			if (candPtrs.rawBlob == 0)
				continue;
			std::vector<float> candVec = toFloat(registry_->loadRawVector(candPtrs.rawBlob));

			bool occluded = false;
			for (size_t i = 0; i < result.size(); ++i) {
				float distP2C = distRaw(candVec, resultVecs[i]);
				if (config_.alpha * distP2C <= candDist) {
					occluded = true;
					break;
				}
			}

			if (!occluded) {
				result.push_back(candId);
				resultVecs.push_back(std::move(candVec));
			}
		}
		candidates = result;
	}

	// --- Distance Helpers ---

	float DiskANNIndex::distRaw(const std::vector<float> &query, int64_t targetId) const {
		const auto ptrs = registry_->getBlobPtrs(targetId);
		if (ptrs.rawBlob == 0)
			return std::numeric_limits<float>::max();
		auto target = registry_->loadRawVector(ptrs.rawBlob);

		if (config_.metric == "IP" || config_.metric == "Cosine") {
			return VectorMetric::computeIP(query.data(), target.data(), target.size());
		}
		// Default L2
		return VectorMetric::computeL2Sqr(query.data(), target.data(), target.size());
	}

	float DiskANNIndex::distRaw(const std::vector<float> &vecA, const std::vector<float> &vecB) {
		return VectorMetric::computeL2Sqr(vecA.data(), vecB.data(), vecA.size());
	}

	float DiskANNIndex::distPQ(const std::vector<float> &pqTable, int64_t targetId) const {
		auto ptrs = registry_->getBlobPtrs(targetId);
		// computeDistance checks for 0, but safety here:
		if (ptrs.pqBlob == 0)
			return std::numeric_limits<float>::max();
		const auto codes = registry_->loadPQCodes(ptrs.pqBlob);
		const size_t M = codes.size();
		const size_t K = pqTable.size() / M;
		return NativeProductQuantizer::computeDistance(codes, pqTable, M, K);
	}

	void DiskANNIndex::remove(int64_t nodeId) const {
		// Logical deletion: Remove from Registry Mapping
		// This effectively makes getBlobPtrs(nodeId) return {0,0,0}
		registry_->setBlobPtrs(nodeId, {0, 0, 0});
	}

} // namespace graph::vector
