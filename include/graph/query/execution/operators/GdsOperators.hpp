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

#include <chrono>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include "../PhysicalOperator.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/query/algorithm/GraphProjectionManager.hpp"
#include "graph/query/algorithm/LeidenEngine.hpp"
#include "graph/query/algorithm/ProjectionSpec.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

	namespace detail {
		template<typename Integer>
		inline int64_t toCatalogInteger(Integer value) noexcept {
			static_assert(std::is_integral_v<Integer>, "Catalog integers must be integral");
			if constexpr (std::is_signed_v<Integer>) {
				if (value < 0) {
					return static_cast<int64_t>(value);
				}
			}
			using Unsigned = std::make_unsigned_t<Integer>;
			const auto unsignedValue = static_cast<Unsigned>(value);
			constexpr auto maxValue = static_cast<Unsigned>((std::numeric_limits<int64_t>::max)());
			return unsignedValue > maxValue ? (std::numeric_limits<int64_t>::max)() : static_cast<int64_t>(value);
		}

		inline std::string orientationToString(algorithm::ProjectionOrientation orientation) {
			switch (orientation) {
				case algorithm::ProjectionOrientation::GPO_NATURAL:
					return "NATURAL";
				case algorithm::ProjectionOrientation::GPO_REVERSE:
					return "REVERSE";
				case algorithm::ProjectionOrientation::GPO_UNDIRECTED:
					return "UNDIRECTED";
			}
			return "NATURAL";
		}

		inline std::string weightKindToString(algorithm::ProjectionWeightKind kind) {
			switch (kind) {
				case algorithm::ProjectionWeightKind::GPWK_NONE:
					return "NONE";
				case algorithm::ProjectionWeightKind::GPWK_CONSTANT:
					return "CONSTANT";
				case algorithm::ProjectionWeightKind::GPWK_PROPERTY:
					return "PROPERTY";
			}
			return "NONE";
		}

		inline PropertyValue stringListValue(const std::vector<std::string> &values) {
			std::vector<PropertyValue> list;
			list.reserve(values.size());
			for (const auto &value : values) {
				list.emplace_back(value);
			}
			return PropertyValue(std::move(list));
		}

		inline std::vector<algorithm::RelationshipProjectionSpec>
		effectiveRelationships(const algorithm::ProjectionSpec &spec) {
			if (!spec.relationships.empty()) {
				return spec.relationships;
			}
			algorithm::RelationshipProjectionSpec allRelationships;
			allRelationships.orientation = spec.defaultOrientation;
			return {std::move(allRelationships)};
		}

		inline PropertyValue relationshipSpecValue(const algorithm::RelationshipProjectionSpec &relationship) {
			PropertyValue::MapType map;
			map.emplace("type", relationship.type);
			map.emplace("orientation", orientationToString(relationship.orientation));
			map.emplace("weightKind", weightKindToString(relationship.weight.kind));
			map.emplace("weightProperty", relationship.weight.propertyName);
			map.emplace("constantWeight", relationship.weight.constantWeight);
			map.emplace("defaultWeight", relationship.weight.defaultWeight);
			return PropertyValue(std::move(map));
		}

		inline PropertyValue relationshipListValue(const algorithm::ProjectionSpec &spec) {
			auto relationships = effectiveRelationships(spec);
			std::vector<PropertyValue> list;
			list.reserve(relationships.size());
			for (const auto &relationship : relationships) {
				list.push_back(relationshipSpecValue(relationship));
			}
			return PropertyValue(std::move(list));
		}

		inline Record descriptorRecord(const algorithm::GraphProjectionDescriptor &descriptor) {
			Record r;
			r.setValue("name", PropertyValue(descriptor.name));
			r.setValue("nodeCount", PropertyValue(toCatalogInteger(descriptor.nodeCount)));
			r.setValue("edgeCount", PropertyValue(toCatalogInteger(descriptor.edgeCount)));
			r.setValue("isWeighted", PropertyValue(descriptor.isWeighted));
			r.setValue("createdAtMillis", PropertyValue(descriptor.createdAtEpochMillis));
			r.setValue("buildMillis", PropertyValue(descriptor.buildMillis));
			r.setValue("memoryBytes", PropertyValue(toCatalogInteger(descriptor.memoryBytes)));
			r.setValue("hasCsr", PropertyValue(descriptor.hasCsr));
			r.setValue("csrMemoryBytes", PropertyValue(toCatalogInteger(descriptor.csrMemoryBytes)));
			r.setValue("sourceRevision", PropertyValue(toCatalogInteger(descriptor.sourceRevision)));
			r.setValue("currentRevision", PropertyValue(toCatalogInteger(descriptor.currentRevision)));
			r.setValue("stale", PropertyValue(descriptor.stale));
			r.setValue("nodeLabels", stringListValue(descriptor.spec.nodeLabels));
			r.setValue("relationships", relationshipListValue(descriptor.spec));
			return r;
		}

		inline Record schemaRecord(const algorithm::GraphProjectionDescriptor &descriptor) {
			Record r;
			r.setValue("name", PropertyValue(descriptor.name));
			r.setValue("nodeLabels", stringListValue(descriptor.spec.nodeLabels));
			r.setValue("relationships", relationshipListValue(descriptor.spec));
			r.setValue("stale", PropertyValue(descriptor.stale));
			return r;
		}
	} // namespace detail

	// ============================================================
	// gds.graph.project
	// ============================================================
	class GdsGraphProjectOperator : public PhysicalOperator {
	public:
		GdsGraphProjectOperator(std::shared_ptr<storage::DataManager> dm,
								std::shared_ptr<algorithm::GraphProjectionManager> pm,
								std::string name, std::string nodeLabel,
								std::string edgeType, std::string weightProperty)
			: GdsGraphProjectOperator(
				std::move(dm), std::move(pm),
				algorithm::ProjectionSpec::legacy(
					std::move(name), std::move(nodeLabel), std::move(edgeType), std::move(weightProperty))) {}

		GdsGraphProjectOperator(std::shared_ptr<storage::DataManager> dm,
								std::shared_ptr<algorithm::GraphProjectionManager> pm,
								algorithm::ProjectionSpec spec)
			: dm_(std::move(dm)), pm_(std::move(pm)), spec_(std::move(spec)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			const auto sourceRevision = dm_->getGraphRevision();
			const auto buildStart = std::chrono::steady_clock::now();
			auto projection = std::make_shared<algorithm::GraphProjection>(
				algorithm::GraphProjection::build(dm_, spec_));
			const auto buildTime = std::chrono::steady_clock::now() - buildStart;

			size_t nodeCount = projection->nodeCount();
			size_t edgeCount = projection->edgeCount();
			pm_->createProjection(spec_, std::move(projection), sourceRevision,
								  std::chrono::duration_cast<std::chrono::nanoseconds>(buildTime));

			RecordBatch batch;
			Record r;
			r.setValue("name", PropertyValue(spec_.name));
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
			return "GdsGraphProject('" + spec_.name + "')";
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		algorithm::ProjectionSpec spec_;
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
	// gds.graph.dropAll
	// ============================================================
	class GdsGraphDropAllOperator : public PhysicalOperator {
	public:
		explicit GdsGraphDropAllOperator(std::shared_ptr<algorithm::GraphProjectionManager> pm)
			: pm_(std::move(pm)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			const size_t dropped = pm_->dropAll();
			RecordBatch batch;
			Record r;
			r.setValue("droppedCount", PropertyValue(detail::toCatalogInteger(dropped)));
			batch.push_back(std::move(r));

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"droppedCount"}; }
		[[nodiscard]] std::string toString() const override { return "GdsGraphDropAll"; }

	private:
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.graph.exists
	// ============================================================
	class GdsGraphExistsOperator : public PhysicalOperator {
	public:
		GdsGraphExistsOperator(std::shared_ptr<algorithm::GraphProjectionManager> pm, std::string name)
			: pm_(std::move(pm)), name_(std::move(name)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			RecordBatch batch;
			Record r;
			r.setValue("name", PropertyValue(name_));
			r.setValue("exists", PropertyValue(pm_->exists(name_)));
			batch.push_back(std::move(r));

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"name", "exists"}; }
		[[nodiscard]] std::string toString() const override { return "GdsGraphExists('" + name_ + "')"; }

	private:
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string name_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.graph.list
	// ============================================================
	class GdsGraphListOperator : public PhysicalOperator {
	public:
		GdsGraphListOperator(std::shared_ptr<storage::DataManager> dm,
							 std::shared_ptr<algorithm::GraphProjectionManager> pm,
							 std::optional<std::string> name)
			: dm_(std::move(dm)), pm_(std::move(pm)), name_(std::move(name)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			RecordBatch batch;
			const auto currentRevision = dm_->getGraphRevision();
			if (name_.has_value()) {
				if (auto descriptor = pm_->describe(*name_, currentRevision)) {
					batch.push_back(detail::descriptorRecord(*descriptor));
				}
			} else {
				for (const auto &descriptor : pm_->list(currentRevision)) {
					batch.push_back(detail::descriptorRecord(descriptor));
				}
			}

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"name", "nodeCount", "edgeCount", "isWeighted", "createdAtMillis", "buildMillis",
					"memoryBytes", "hasCsr", "csrMemoryBytes", "sourceRevision", "currentRevision",
					"stale", "nodeLabels", "relationships"};
		}
		[[nodiscard]] std::string toString() const override {
			return name_.has_value() ? "GdsGraphList('" + *name_ + "')" : "GdsGraphList";
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::optional<std::string> name_;
		bool executed_ = false;
	};

	// ============================================================
	// gds.graph.schema
	// ============================================================
	class GdsGraphSchemaOperator : public PhysicalOperator {
	public:
		GdsGraphSchemaOperator(std::shared_ptr<storage::DataManager> dm,
							   std::shared_ptr<algorithm::GraphProjectionManager> pm,
							   std::string name)
			: dm_(std::move(dm)), pm_(std::move(pm)), name_(std::move(name)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			auto descriptor = pm_->describe(name_, dm_->getGraphRevision());
			if (!descriptor.has_value()) {
				throw std::runtime_error("Graph projection '" + name_ + "' not found");
			}

			RecordBatch batch;
			batch.push_back(detail::schemaRecord(*descriptor));
			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"name", "nodeLabels", "relationships", "stale"};
		}
		[[nodiscard]] std::string toString() const override { return "GdsGraphSchema('" + name_ + "')"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
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

	// ============================================================
	// gds.leiden.stream
	// ============================================================
	class GdsLeidenOperator : public PhysicalOperator {
	public:
		GdsLeidenOperator(std::shared_ptr<storage::DataManager> dm,
						  std::shared_ptr<algorithm::GraphProjectionManager> pm,
						  std::string graphName, int maxIterations = 20, double resolution = 1.0,
						  size_t threadCount = 0, int maxLevels = 10,
						  double refinementThreshold = 0.01)
			: dm_(std::move(dm)), pm_(std::move(pm)), graphName_(std::move(graphName)),
			  maxIterations_(maxIterations), resolution_(resolution), threadCount_(threadCount),
			  maxLevels_(maxLevels), refinementThreshold_(refinementThreshold) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			// Short-lived pool for this run; reused for both CSR build and the
			// Leiden local-moving / refinement / aggregation pipeline.
			std::unique_ptr<concurrent::ThreadPool> ownedPool;
			concurrent::ThreadPool *pool = nullptr;
			if (threadCount_ != 1) {
				ownedPool = std::make_unique<concurrent::ThreadPool>(threadCount_);
				pool = ownedPool.get();
			}

			auto csr = pm_->getOrBuildCsr(graphName_, pool);
			algorithm::LeidenOptions opts;
			opts.maxIterations = maxIterations_;
			opts.maxLevels = maxLevels_;
			opts.resolution = resolution_;
			opts.refinementThreshold = refinementThreshold_;
			auto communities = algorithm::LeidenEngine::run(*csr, opts, pool);

			RecordBatch batch;
			batch.reserve(communities.size());
			for (const auto &nc : communities) {
				Record r;
				r.setValue("nodeId", PropertyValue(nc.nodeId));
				r.setValue("communityId", PropertyValue(nc.communityId));
				batch.push_back(std::move(r));
			}

			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"nodeId", "communityId"};
		}
		[[nodiscard]] std::string toString() const override { return "GdsLeiden('" + graphName_ + "')"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<algorithm::GraphProjectionManager> pm_;
		std::string graphName_;
		int maxIterations_;
		double resolution_;
		size_t threadCount_;
		int maxLevels_;
		double refinementThreshold_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
