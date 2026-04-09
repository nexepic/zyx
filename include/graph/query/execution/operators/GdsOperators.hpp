/**
 * @file GdsOperators.hpp
 * @author Nexepic
 * @date 2026/4/9
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

#include "../PhysicalOperator.hpp"
#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/query/algorithm/GraphProjectionManager.hpp"

namespace graph::query::execution::operators {

	// ============================================================
	// gds.graph.project
	// ============================================================
	class GdsGraphProjectOperator : public PhysicalOperator {
	public:
		GdsGraphProjectOperator(std::shared_ptr<storage::DataManager> dm,
								std::shared_ptr<algorithm::GraphProjectionManager> pm,
								std::string name, std::string nodeLabel,
								std::string edgeType, std::string weightProperty)
			: dm_(std::move(dm)), pm_(std::move(pm)), name_(std::move(name)),
			  nodeLabel_(std::move(nodeLabel)), edgeType_(std::move(edgeType)),
			  weightProperty_(std::move(weightProperty)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			auto projection = std::make_shared<algorithm::GraphProjection>(
				algorithm::GraphProjection::build(dm_, nodeLabel_, edgeType_, weightProperty_));

			size_t nodeCount = projection->nodeCount();
			size_t edgeCount = projection->edgeCount();
			pm_->createProjection(name_, std::move(projection));

			RecordBatch batch;
			Record r;
			r.setValue("name", PropertyValue(name_));
			r.setValue("nodeCount", PropertyValue(static_cast<int64_t>(nodeCount)));
			r.setValue("edgeCount", PropertyValue(static_cast<int64_t>(edgeCount)));
			batch.push_back(std::move(r));

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"name", "nodeCount", "edgeCount"};
		}
		[[nodiscard]] std::string toString() const override {
			return "GdsGraphProject('" + name_ + "')";
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string name_, nodeLabel_, edgeType_, weightProperty_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.graph.drop
	// ============================================================
	class GdsGraphDropOperator : public PhysicalOperator {
	public:
		GdsGraphDropOperator(std::shared_ptr<algorithm::GraphProjectionManager> pm, std::string name)
			: pm_(std::move(pm)), name_(std::move(name)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			if (!pm_->dropProjection(name_)) {
				throw std::runtime_error("Graph projection '" + name_ + "' not found");
			}

			RecordBatch batch;
			Record r;
			r.setValue("name", PropertyValue(name_));
			batch.push_back(std::move(r));

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"name"}; }
		[[nodiscard]] std::string toString() const override { return "GdsGraphDrop('" + name_ + "')"; }

	private:
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string name_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.shortestPath.dijkstra.stream
	// ============================================================
	class GdsDijkstraOperator : public PhysicalOperator {
	public:
		GdsDijkstraOperator(std::shared_ptr<storage::DataManager> dm,
							std::shared_ptr<algorithm::GraphProjectionManager> pm,
							std::string graphName, int64_t startId, int64_t endId)
			: dm_(std::move(dm)), pm_(std::move(pm)), graphName_(std::move(graphName)),
			  startId_(startId), endId_(endId) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			auto projection = pm_->getProjection(graphName_);
			algorithm::GraphAlgorithm algo(dm_);
			auto result = algo.dijkstra(*projection, startId_, endId_);

			if (result.nodes.empty()) return std::nullopt;

			RecordBatch batch;
			double accumulatedCost = 0.0;
			for (size_t i = 0; i < result.nodes.size(); ++i) {
				Record r;
				r.setValue("nodeId", PropertyValue(result.nodes[i].getId()));
				r.setNode("node", result.nodes[i]);
				r.setValue("cost", PropertyValue(accumulatedCost));
				r.setValue("totalCost", PropertyValue(result.totalWeight));
				batch.push_back(std::move(r));
				// Approximate per-step cost (for display purposes)
				if (i + 1 < result.nodes.size()) {
					accumulatedCost = result.totalWeight *
						static_cast<double>(i + 1) / static_cast<double>(result.nodes.size() - 1);
				}
			}

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"nodeId", "node", "cost", "totalCost"};
		}
		[[nodiscard]] std::string toString() const override {
			return "GdsDijkstra(" + std::to_string(startId_) + " -> " + std::to_string(endId_) + ")";
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string graphName_;
		int64_t startId_, endId_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.pageRank.stream
	// ============================================================
	class GdsPageRankOperator : public PhysicalOperator {
	public:
		GdsPageRankOperator(std::shared_ptr<storage::DataManager> dm,
							std::shared_ptr<algorithm::GraphProjectionManager> pm,
							std::string graphName, int maxIterations = 20, double dampingFactor = 0.85)
			: dm_(std::move(dm)), pm_(std::move(pm)), graphName_(std::move(graphName)),
			  maxIterations_(maxIterations), dampingFactor_(dampingFactor) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			auto projection = pm_->getProjection(graphName_);
			algorithm::GraphAlgorithm algo(dm_);
			auto scores = algo.pageRank(*projection, maxIterations_, dampingFactor_);

			if (scores.empty()) return std::nullopt;

			RecordBatch batch;
			for (const auto &ns : scores) {
				Record r;
				r.setValue("nodeId", PropertyValue(ns.nodeId));
				r.setValue("score", PropertyValue(ns.score));
				batch.push_back(std::move(r));
			}

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"nodeId", "score"}; }
		[[nodiscard]] std::string toString() const override { return "GdsPageRank('" + graphName_ + "')"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string graphName_;
		int maxIterations_;
		double dampingFactor_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.wcc.stream
	// ============================================================
	class GdsWccOperator : public PhysicalOperator {
	public:
		GdsWccOperator(std::shared_ptr<storage::DataManager> dm,
					   std::shared_ptr<algorithm::GraphProjectionManager> pm,
					   std::string graphName)
			: dm_(std::move(dm)), pm_(std::move(pm)), graphName_(std::move(graphName)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			auto projection = pm_->getProjection(graphName_);
			algorithm::GraphAlgorithm algo(dm_);
			auto components = algo.connectedComponents(*projection);

			if (components.empty()) return std::nullopt;

			RecordBatch batch;
			for (const auto &nc : components) {
				Record r;
				r.setValue("nodeId", PropertyValue(nc.nodeId));
				r.setValue("componentId", PropertyValue(nc.componentId));
				batch.push_back(std::move(r));
			}

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"nodeId", "componentId"};
		}
		[[nodiscard]] std::string toString() const override { return "GdsWCC('" + graphName_ + "')"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string graphName_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.betweenness.stream
	// ============================================================
	class GdsBetweennessOperator : public PhysicalOperator {
	public:
		GdsBetweennessOperator(std::shared_ptr<storage::DataManager> dm,
							   std::shared_ptr<algorithm::GraphProjectionManager> pm,
							   std::string graphName, int samplingSize = 0)
			: dm_(std::move(dm)), pm_(std::move(pm)), graphName_(std::move(graphName)),
			  samplingSize_(samplingSize) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			auto projection = pm_->getProjection(graphName_);
			algorithm::GraphAlgorithm algo(dm_);
			auto scores = algo.betweennessCentrality(*projection, samplingSize_);

			if (scores.empty()) return std::nullopt;

			RecordBatch batch;
			for (const auto &ns : scores) {
				Record r;
				r.setValue("nodeId", PropertyValue(ns.nodeId));
				r.setValue("score", PropertyValue(ns.score));
				batch.push_back(std::move(r));
			}

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"nodeId", "score"}; }
		[[nodiscard]] std::string toString() const override { return "GdsBetweenness('" + graphName_ + "')"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string graphName_;
		int samplingSize_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.closeness.stream
	// ============================================================
	class GdsClosenessOperator : public PhysicalOperator {
	public:
		GdsClosenessOperator(std::shared_ptr<storage::DataManager> dm,
							 std::shared_ptr<algorithm::GraphProjectionManager> pm,
							 std::string graphName)
			: dm_(std::move(dm)), pm_(std::move(pm)), graphName_(std::move(graphName)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			auto projection = pm_->getProjection(graphName_);
			algorithm::GraphAlgorithm algo(dm_);
			auto scores = algo.closenessCentrality(*projection);

			if (scores.empty()) return std::nullopt;

			RecordBatch batch;
			for (const auto &ns : scores) {
				Record r;
				r.setValue("nodeId", PropertyValue(ns.nodeId));
				r.setValue("score", PropertyValue(ns.score));
				batch.push_back(std::move(r));
			}

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"nodeId", "score"}; }
		[[nodiscard]] std::string toString() const override { return "GdsCloseness('" + graphName_ + "')"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string graphName_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
