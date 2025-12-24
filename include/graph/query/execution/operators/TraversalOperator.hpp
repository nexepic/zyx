/**
 * @file TraversalOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

	class TraversalOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a traversal operator.
		 *
		 * @param dm DataManager for retrieving edges and nodes.
		 * @param child Upstream operator providing source nodes.
		 * @param sourceVar Variable name of the start node (already in record).
		 * @param edgeVar Variable name for the edge to be found.
		 * @param targetVar Variable name for the target node to be found.
		 * @param edgeLabel Filter by edge label (empty means all labels).
		 * @param direction "out", "in", or "both".
		 */
		TraversalOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
						  std::string sourceVar, std::string edgeVar, std::string targetVar, std::string edgeLabel,
						  std::string direction) :
			dm_(std::move(dm)), child_(std::move(child)), sourceVar_(std::move(sourceVar)),
			edgeVar_(std::move(edgeVar)), targetVar_(std::move(targetVar)), edgeLabel_(std::move(edgeLabel)),
			direction_(std::move(direction)) {}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			// 1. Pull batch from upstream (Source Nodes)
			auto batchOpt = child_->next();
			if (!batchOpt)
				return std::nullopt;

			RecordBatch &inputBatch = *batchOpt;
			RecordBatch outputBatch;

			// Output can be larger than input (1-to-N relationship), so reserve conservatively
			outputBatch.reserve(inputBatch.size() * 2);

			for (const auto &record: inputBatch) {
				// 2. Get Source Node
				auto sourceNodeOpt = record.getNode(sourceVar_);
				if (!sourceNodeOpt)
					continue;
				int64_t sourceId = sourceNodeOpt->getId();

				// 3. Find Connected Edges (Low-level Traversal)
				// DataManager::findEdgesByNode calls RelationshipTraversal internally
				std::vector<Edge> edges = dm_->findEdgesByNode(sourceId, direction_);

				for (auto &edge: edges) {
					// 4. Filter by Edge Label
					if (!edgeLabel_.empty() && edge.getLabel() != edgeLabel_) {
						continue;
					}

					// 5. Hydrate Edge Properties
					auto edgeProps = dm_->getEdgeProperties(edge.getId());
					edge.setProperties(std::move(edgeProps));

					// 6. Resolve Target Node
					// Determine which side is the "other" side
					int64_t targetNodeId =
							(edge.getSourceNodeId() == sourceId) ? edge.getTargetNodeId() : edge.getSourceNodeId();

					Node targetNode = dm_->getNode(targetNodeId);
					if (!targetNode.isActive())
						continue;

					// 7. Hydrate Target Node Properties
					auto nodeProps = dm_->getNodeProperties(targetNodeId);
					targetNode.setProperties(std::move(nodeProps));

					// 8. Create Extended Record
					Record newRecord = record; // Copy existing variables
					newRecord.setEdge(edgeVar_, edge);
					newRecord.setNode(targetVar_, targetNode);

					outputBatch.push_back(std::move(newRecord));
				}
			}

			// If batch was filtered out completely, we might need to fetch next
			// For simplicity, we return empty batch or check recursively in a robust impl.
			// Here we return whatever we found.
			return outputBatch;
		}

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_->getOutputVariables();
			vars.push_back(edgeVar_);
			vars.push_back(targetVar_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override {
			return "Traversal(" + sourceVar_ + " - [" + edgeVar_ + ":" + edgeLabel_ + "] -> " + targetVar_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_)
				return {child_.get()};
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;

		std::string sourceVar_;
		std::string edgeVar_;
		std::string targetVar_;
		std::string edgeLabel_;
		std::string direction_;
	};

} // namespace graph::query::execution::operators
