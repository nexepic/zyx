/**
 * @file VarLengthTraversalOperator.hpp
 * @author Nexepic
 * @date 2025/12/22
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include <stack>
#include <unordered_set>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

	class VarLengthTraversalOperator : public PhysicalOperator {
	public:
		VarLengthTraversalOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
								   std::string sourceVar, std::string targetVar, std::string edgeType, int minHops,
								   int maxHops, std::string direction) :
			dm_(std::move(dm)), child_(std::move(child)), sourceVar_(std::move(sourceVar)),
			targetVar_(std::move(targetVar)),
			edgeType_(std::move(edgeType)), direction_(std::move(direction)), minHops_(minHops), maxHops_(maxHops) {}

		void open() override {
			if (child_)
				child_->open();

			// Resolve edge type ID once
			edgeTypeId_ = 0;
			if (!edgeType_.empty()) {
				edgeTypeId_ = dm_->resolveTokenId(edgeType_);
				if (edgeTypeId_ == 0) edgeTypeId_ = -1;
			}

			// Clamp maxHops to runtime config limit
			if (queryContext_) {
				maxHops_ = std::min(maxHops_, queryContext_->maxVarLengthDepth);
			}

			// Reset DFS state
			while (!dfsStack_.empty()) dfsStack_.pop();
			visitedPath_.clear();
			currentInputBatch_.reset();
			inputIdx_ = 0;
			inputExhausted_ = false;
		}

		std::optional<RecordBatch> next() override {
			RecordBatch outputBatch;
			outputBatch.reserve(DEFAULT_BATCH_SIZE);

			while (outputBatch.size() < DEFAULT_BATCH_SIZE) {
				// If DFS stack is empty, pull next input record
				if (dfsStack_.empty()) {
					if (!advanceInput()) {
						break; // No more input
					}
					// Push source node as initial frame
					auto sourceNode = currentInputRecord_.getNode(sourceVar_);
					if (!sourceNode) continue;

					int64_t sourceId = sourceNode->getId();
					dfsStack_.push({sourceId, 0, {}, 0, false});
					visitedPath_.clear();
					visitedPath_.insert(sourceId);
				}

				// Process DFS
				while (!dfsStack_.empty() && outputBatch.size() < DEFAULT_BATCH_SIZE) {
					if (queryContext_) queryContext_->checkGuard();

					auto &frame = dfsStack_.top();

					// Lazy-load neighbors
					if (!frame.neighborsLoaded) {
						frame.neighborsLoaded = true;
						frame.neighbors = getNeighborIds(frame.nodeId);
						frame.neighborIdx = 0;
					}

					// If depth >= minHops, emit this node as a result
					if (frame.depth >= minHops_ && frame.depth <= maxHops_ && !frame.emitted) {
						frame.emitted = true;
						// Only emit non-source nodes (depth > 0) or if minHops is 0
						if (frame.depth > 0 || minHops_ == 0) {
							Node targetNode = dm_->getNode(frame.nodeId);
							if (targetNode.isActive()) {
								auto props = dm_->getNodeProperties(frame.nodeId);
								targetNode.setProperties(std::move(props));
								Record newRecord = currentInputRecord_;
								newRecord.setNode(targetVar_, targetNode);
								outputBatch.push_back(std::move(newRecord));
							}
						}
					}

					// Try to go deeper
					if (frame.depth < maxHops_) {
						bool pushed = false;
						while (frame.neighborIdx < frame.neighbors.size()) {
							int64_t neighborId = frame.neighbors[frame.neighborIdx++];
							// Cycle detection: skip if already on current path
							if (visitedPath_.count(neighborId)) continue;

							// Check node is active
							Node neighborNode = dm_->getNode(neighborId);
							if (!neighborNode.isActive()) continue;

							visitedPath_.insert(neighborId);
							dfsStack_.push({neighborId, frame.depth + 1, {}, 0, false});
							pushed = true;
							break;
						}

						if (!pushed) {
							// No more neighbors — backtrack
							visitedPath_.erase(frame.nodeId);
							dfsStack_.pop();
						}
					} else {
						// At max depth — backtrack
						visitedPath_.erase(frame.nodeId);
						dfsStack_.pop();
					}
				}
			}

			if (outputBatch.empty()) return std::nullopt;
			return outputBatch;
		}

		void close() override {
			if (child_)
				child_->close();
			while (!dfsStack_.empty()) dfsStack_.pop();
			visitedPath_.clear();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto v = child_->getOutputVariables();
			v.push_back(targetVar_);
			return v;
		}

		[[nodiscard]] std::string toString() const override {
			return "VarLengthTraversal(" + sourceVar_ + " -[*" + std::to_string(minHops_) + ".." +
				   std::to_string(maxHops_) + "]-> " + targetVar_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {child_.get()}; }

	private:
		struct DFSFrame {
			int64_t nodeId;
			int depth;
			std::vector<int64_t> neighbors;
			size_t neighborIdx;
			bool neighborsLoaded;
			bool emitted = false;
		};

		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;
		std::string sourceVar_, targetVar_, edgeType_, direction_;
		int minHops_, maxHops_;
		int64_t edgeTypeId_ = 0;

		// DFS state
		std::stack<DFSFrame> dfsStack_;
		std::unordered_set<int64_t> visitedPath_;
		std::optional<RecordBatch> currentInputBatch_;
		size_t inputIdx_ = 0;
		Record currentInputRecord_;
		bool inputExhausted_ = false;

		bool advanceInput() {
			while (true) {
				if (currentInputBatch_ && inputIdx_ < currentInputBatch_->size()) {
					currentInputRecord_ = (*currentInputBatch_)[inputIdx_++];
					return true;
				}
				if (inputExhausted_) return false;

				auto batch = child_->next();
				if (!batch) {
					inputExhausted_ = true;
					return false;
				}
				currentInputBatch_ = std::move(batch);
				inputIdx_ = 0;
			}
		}

		std::vector<int64_t> getNeighborIds(int64_t nodeId) {
			std::vector<int64_t> neighbors;
			auto edges = dm_->findEdgesByNode(nodeId, direction_);

			neighbors.reserve(edges.size());
			for (const auto &edge : edges) {
				if (edgeTypeId_ != 0) {
					if (edge.getTypeId() != edgeTypeId_) continue;
				}
				int64_t neighborId = (edge.getSourceNodeId() == nodeId)
					? edge.getTargetNodeId() : edge.getSourceNodeId();
				neighbors.push_back(neighborId);
			}
			return neighbors;
		}
	};

} // namespace graph::query::execution::operators
