/**
 * @file MergeEdgeOperator.hpp
 * @author ZYX Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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

#include "../PhysicalOperator.hpp"
#include "SetOperator.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"

namespace graph::query::execution::operators {

	/**
	 * @class MergeEdgeOperator
	 * @brief Implements MERGE (a)-[r:TYPE {props}]->(b) for edge patterns.
	 *
	 * This operator expects source and target nodes to already be resolved
	 * in the input record (via preceding MergeNodeOperator calls).
	 * It then merges (finds or creates) the edge between them.
	 */
	class MergeEdgeOperator : public PhysicalOperator {
	public:
		MergeEdgeOperator(std::shared_ptr<storage::DataManager> dm,
		                  std::shared_ptr<indexes::IndexManager> im,
		                  std::string sourceVar, std::string edgeVar, std::string targetVar,
		                  std::string edgeLabel,
		                  std::unordered_map<std::string, PropertyValue> matchProps,
		                  std::string direction,
		                  std::vector<SetItem> onCreateItems,
		                  std::vector<SetItem> onMatchItems) :
			dm_(std::move(dm)), im_(std::move(im)),
			sourceVar_(std::move(sourceVar)), edgeVar_(std::move(edgeVar)),
			targetVar_(std::move(targetVar)), edgeLabel_(std::move(edgeLabel)),
			matchProps_(std::move(matchProps)), direction_(std::move(direction)),
			onCreateItems_(std::move(onCreateItems)), onMatchItems_(std::move(onMatchItems)) {}

		void setChild(std::unique_ptr<PhysicalOperator> child) { child_ = std::move(child); }

		void open() override {
			executed_ = false;
			if (child_) child_->open();
			edgeLabelId_ = 0;
			if (!edgeLabel_.empty()) {
				edgeLabelId_ = dm_->getOrCreateLabelId(edgeLabel_);
			}
		}

		std::optional<RecordBatch> next() override {
			if (child_) {
				auto batchOpt = child_->next();
				if (!batchOpt) return std::nullopt;

				RecordBatch outputBatch;
				outputBatch.reserve(batchOpt->size());

				for (const auto &record : *batchOpt) {
					Record newRecord = record;
					processMergeEdge(newRecord);
					outputBatch.push_back(std::move(newRecord));
				}
				return outputBatch;
			}

			if (executed_) return std::nullopt;
			executed_ = true;

			Record record;
			processMergeEdge(record);

			RecordBatch batch;
			batch.push_back(std::move(record));
			return batch;
		}

		void close() override { if (child_) child_->close(); }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
			if (!edgeVar_.empty()) vars.push_back(edgeVar_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override {
			return "MergeEdge(" + sourceVar_ + ")-[" + edgeVar_ + ":" + edgeLabel_ + "]->(" + targetVar_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_) return {child_.get()};
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		std::unique_ptr<PhysicalOperator> child_;

		std::string sourceVar_;
		std::string edgeVar_;
		std::string targetVar_;
		std::string edgeLabel_;
		std::unordered_map<std::string, PropertyValue> matchProps_;
		std::string direction_;

		std::vector<SetItem> onCreateItems_;
		std::vector<SetItem> onMatchItems_;

		bool executed_ = false;
		int64_t edgeLabelId_ = 0;

		void processMergeEdge(Record &record) {
			// Get source and target node IDs from the record
			auto sourceNode = record.getNode(sourceVar_);
			auto targetNode = record.getNode(targetVar_);

			if (!sourceNode || !targetNode) {
				throw std::runtime_error("MERGE edge requires source (" + sourceVar_ +
				                         ") and target (" + targetVar_ + ") nodes to be resolved");
			}

			int64_t sourceId = sourceNode->getId();
			int64_t targetId = targetNode->getId();

			// Try to find existing edge
			int64_t foundEdgeId = 0;
			auto edges = dm_->findEdgesByNode(sourceId, "both");

			for (const auto &edge : edges) {
				if (!edge.isActive()) continue;

				// Check direction
				bool dirMatch = false;
				if (direction_ == "out") {
					dirMatch = (edge.getSourceNodeId() == sourceId && edge.getTargetNodeId() == targetId);
				} else if (direction_ == "in") {
					dirMatch = (edge.getSourceNodeId() == targetId && edge.getTargetNodeId() == sourceId);
				} else {
					dirMatch = (edge.getSourceNodeId() == sourceId && edge.getTargetNodeId() == targetId) ||
					           (edge.getSourceNodeId() == targetId && edge.getTargetNodeId() == sourceId);
				}
				if (!dirMatch) continue;

				// Check label
				if (edgeLabelId_ != 0 && edge.getLabelId() != edgeLabelId_) continue;

				// Check properties
				auto edgeProps = dm_->getEdgeProperties(edge.getId());
				bool match = true;
				for (const auto &[k, v] : matchProps_) {
					auto it = edgeProps.find(k);
					if (it == edgeProps.end() || it->second != v) {
						match = false;
						break;
					}
				}

				if (match) {
					foundEdgeId = edge.getId();
					if (!edgeVar_.empty()) {
						Edge matchedEdge = edge;
						matchedEdge.setProperties(std::move(edgeProps));
						record.setEdge(edgeVar_, matchedEdge);
					}
					break;
				}
			}

			if (foundEdgeId != 0) {
				// MATCHED - apply ON MATCH
				applyEdgeUpdates(foundEdgeId, onMatchItems_, record);
			} else {
				// NOT MATCHED - CREATE
				int64_t realSourceId = sourceId;
				int64_t realTargetId = targetId;
				if (direction_ == "in") {
					std::swap(realSourceId, realTargetId);
				}

				Edge newEdge(0, realSourceId, realTargetId, edgeLabelId_);
				dm_->addEdge(newEdge);

				if (!matchProps_.empty()) {
					dm_->addEdgeProperties(newEdge.getId(), matchProps_);
				}

				newEdge.setProperties(matchProps_);
				if (!edgeVar_.empty()) {
					record.setEdge(edgeVar_, newEdge);
				}

				applyEdgeUpdates(newEdge.getId(), onCreateItems_, record);
			}
		}

		void applyEdgeUpdates(int64_t edgeId, const std::vector<SetItem> &items, Record &record) {
			if (items.empty()) return;

			auto props = dm_->getEdgeProperties(edgeId);
			bool changed = false;

			for (const auto &item : items) {
				if (item.variable == edgeVar_ && item.type == SetActionType::PROPERTY && item.expression) {
					graph::query::expressions::EvaluationContext context(record);
					graph::query::expressions::ExpressionEvaluator evaluator(context);
					PropertyValue value = evaluator.evaluate(item.expression.get());
					props[item.key] = value;
					changed = true;
				}
			}

			if (changed) {
				dm_->addEdgeProperties(edgeId, props);
				if (!edgeVar_.empty()) {
					if (auto e = record.getEdge(edgeVar_)) {
						Edge updatedEdge = *e;
						updatedEdge.setProperties(std::move(props));
						record.setEdge(edgeVar_, updatedEdge);
					}
				}
			}
		}
	};

} // namespace graph::query::execution::operators
