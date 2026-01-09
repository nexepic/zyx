/**
 * @file MergeNodeOperator.hpp
 * @author Nexepic
 * @date 2025/12/21
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

#include "../PhysicalOperator.hpp"
#include "SetOperator.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class MergeNodeOperator : public PhysicalOperator {
	public:
		MergeNodeOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
						  std::string variable, std::string label,
						  std::unordered_map<std::string, PropertyValue> matchProps, std::vector<SetItem> onCreateItems,
						  std::vector<SetItem> onMatchItems) :
			dm_(std::move(dm)), im_(std::move(im)), variable_(std::move(variable)), label_(std::move(label)),
			matchProps_(std::move(matchProps)), onCreateItems_(std::move(onCreateItems)),
			onMatchItems_(std::move(onMatchItems)) {}

		void setChild(std::unique_ptr<PhysicalOperator> child) { child_ = std::move(child); }

		void open() override {
			executed_ = false;
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			// 1. Pipeline Mode
			if (child_) {
				auto batchOpt = child_->next();
				if (!batchOpt)
					return std::nullopt;

				RecordBatch outputBatch;
				outputBatch.reserve(batchOpt->size());

				for (const auto &record: *batchOpt) {
					Record newRecord = record;
					processMerge(newRecord);
					outputBatch.push_back(std::move(newRecord));
				}
				return outputBatch;
			}

			// 2. Source Mode
			if (executed_)
				return std::nullopt;

			Record record;
			processMerge(record);

			RecordBatch batch;
			batch.push_back(std::move(record));
			executed_ = true;
			return batch;
		}

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
			vars.push_back(variable_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override { return "MergeNode(" + variable_ + ":" + label_ + ")"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		std::unique_ptr<PhysicalOperator> child_;

		std::string variable_;
		std::string label_;
		std::unordered_map<std::string, PropertyValue> matchProps_;

		std::vector<SetItem> onCreateItems_;
		std::vector<SetItem> onMatchItems_;

		bool executed_ = false;

		// --- Core Logic ---
		void processMerge(Record &record) {
			// 1. Check if variable is already bound (Pattern Check)
			if (record.getNode(variable_))
				return;

			// 2. Try to find existing node
			// Strategy: Use Index if available, otherwise Full Scan (simplified here)
			// Note: In production, this "Find" logic should be shared with NodeScanOperator or Optimizer.
			// Here we implement a simple lookup loop.

			std::vector<int64_t> candidates;
			bool indexUsed = false;

			// Try to find a property to index scan
			for (const auto &[key, val]: matchProps_) {
				if (im_->hasPropertyIndex("node", key)) {
					candidates = im_->findNodeIdsByProperty(key, val);
					indexUsed = true;
					break;
				}
			}

			// If no property index, try label index
			if (!indexUsed && im_->hasLabelIndex("node")) {
				candidates = im_->findNodeIdsByLabel(label_);
				indexUsed = true;
			}

			// If still no index, we must full scan (Expensive! Warn in logs)
			if (!indexUsed) {
				// Fallback: Scan all nodes (Simplified: Assuming ID range)
				int64_t maxId = dm_->getIdAllocator()->getCurrentMaxNodeId();
				for (int64_t i = 1; i <= maxId; ++i)
					candidates.push_back(i);
			}

			// Filter candidates
			int64_t foundId = 0;
			for (int64_t id: candidates) {
				Node n = dm_->getNode(id);
				if (!n.isActive())
					continue;
				if (!label_.empty() && n.getLabel() != label_)
					continue;

				// Hydrate & Check Properties
				auto props = dm_->getNodeProperties(id);
				bool match = true;
				for (const auto &[k, v]: matchProps_) {
					auto it = props.find(k);
					if (it == props.end() || it->second != v) {
						match = false;
						break;
					}
				}

				if (match) {
					foundId = id;
					// Hydrate object for record
					n.setProperties(std::move(props));
					record.setNode(variable_, n);
					break; // Found match (Single)
				}
			}

			// 3. Action
			if (foundId != 0) {
				// === MATCHED ===
				applyUpdates(foundId, onMatchItems_, record);
			} else {
				// === NOT MATCHED -> CREATE ===
				Node newNode(0, label_);
				dm_->addNode(newNode);

				// Add base properties from MERGE pattern
				if (!matchProps_.empty()) {
					dm_->addNodeProperties(newNode.getId(), matchProps_);
				}

				// Hydrate base
				newNode.setProperties(matchProps_);
				record.setNode(variable_, newNode); // Bind early for SetOperator logic

				// Apply ON CREATE updates
				applyUpdates(newNode.getId(), onCreateItems_, record);
			}
		}

		void applyUpdates(int64_t nodeId, const std::vector<SetItem> &items, Record &record) {
			if (items.empty())
				return;

			// Read-Modify-Write
			auto props = dm_->getNodeProperties(nodeId);
			bool changed = false;

			for (const auto &item: items) {
				// Ensure the SET targets this variable
				if (item.variable == variable_) {
					props[item.key] = item.value;
					changed = true;
				}
			}

			if (changed) {
				dm_->addNodeProperties(nodeId, props);
				// Update record object
				if (auto n = record.getNode(variable_)) {
					Node updatedNode = *n;
					updatedNode.setProperties(std::move(props));
					record.setNode(variable_, updatedNode);
				}
			}
		}
	};

} // namespace graph::query::execution::operators
