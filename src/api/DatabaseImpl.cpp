/**
 * @file DatabaseImpl.cpp
 * @author Nexepic
 * @date 2025/12/16
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

#include <algorithm>
#include "graph/core/Database.hpp"
#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include "graph/query/api/QueryBuilder.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "metrix/metrix.hpp"

namespace metrix {

	// ========================================================================
	// 1. Helpers: Type Conversion
	// ========================================================================

	// Forward declaration
	Value toPublicValue(const graph::query::ResultValue &v, const std::shared_ptr<graph::storage::DataManager> &dm);
	Value toPublicValue(const graph::PropertyValue &v);

	// Convert Internal Node to Public Node Pointer (For shared_ptr usage in Value)
	std::shared_ptr<Node> toPublicNodePtr(const graph::Node &internalNode,
										  const std::shared_ptr<graph::storage::DataManager> &dm) {
		auto pubNode = std::make_shared<Node>();
		pubNode->id = internalNode.getId();

		// RESOLVE: ID -> String using DataManager
		if (dm) {
			pubNode->label = dm->resolveLabel(internalNode.getLabelId());
		}

		for (const auto &[key, val]: internalNode.getProperties()) {
			pubNode->properties[key] = toPublicValue(val);
		}
		return pubNode;
	}

	// Convert Internal Edge to Public Edge Pointer
	std::shared_ptr<Edge> toPublicEdgePtr(const graph::Edge &internalEdge,
										  const std::shared_ptr<graph::storage::DataManager> &dm) {
		auto pubEdge = std::make_shared<Edge>();
		pubEdge->id = internalEdge.getId();
		pubEdge->sourceId = internalEdge.getSourceNodeId();
		pubEdge->targetId = internalEdge.getTargetNodeId();

		// RESOLVE: ID -> String using DataManager
		if (dm) {
			pubEdge->label = dm->resolveLabel(internalEdge.getLabelId());
		}

		for (const auto &[key, val]: internalEdge.getProperties()) {
			pubEdge->properties[key] = toPublicValue(val);
		}
		return pubEdge;
	}

	// Convert Internal Node to Public Node (Value Copy for Algorithms)
	Node toPublicNode(const graph::Node &internalNode, const std::shared_ptr<graph::storage::DataManager> &dm) {
		auto ptr = toPublicNodePtr(internalNode, dm);
		return *ptr; // Copy the struct
	}

	// Convert internal PropertyValue to public Value variant
	Value toPublicValue(const graph::PropertyValue &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						return std::monostate{};
					} else {
						// Primitives (int, double, bool, string) map directly
						return arg;
					}
				},
				v.getVariant());
	}

	// Main conversion entry point for ResultValue
	Value toPublicValue(const graph::query::ResultValue &v, const std::shared_ptr<graph::storage::DataManager> &dm) {
		return std::visit(
				[&dm]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, graph::Node>) {
						return toPublicNodePtr(arg, dm);
					} else if constexpr (std::is_same_v<T, graph::Edge>) {
						return toPublicEdgePtr(arg, dm);
					} else if constexpr (std::is_same_v<T, graph::PropertyValue>) {
						return toPublicValue(arg);
					} else {
						return std::monostate{};
					}
				},
				v.getVariant());
	}

	// Convert public API Value to internal PropertyValue
	graph::PropertyValue toInternal(const Value &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> graph::PropertyValue {
					using T = std::decay_t<T0>;
					// Complex types in Public API (shared_ptr) cannot be stored directly in PropertyValue
					if constexpr (std::is_same_v<T, std::shared_ptr<Node>> ||
								  std::is_same_v<T, std::shared_ptr<Edge>> ||
								  std::is_same_v<T, std::vector<std::string>>) {
						return {}; // Null/Empty for unsupported types
					} else {
						return graph::PropertyValue(arg);
					}
				},
				v);
	}

	// ========================================================================
	// 2. Result Implementation
	// ========================================================================

	class ResultImpl {
	public:
		explicit ResultImpl(graph::query::QueryResult internalRes, std::shared_ptr<graph::storage::DataManager> dm) :
			result_(std::move(internalRes)), dataManager_(std::move(dm)) {
			prepareMetadata();
		}

		graph::query::QueryResult result_;
		std::shared_ptr<graph::storage::DataManager> dataManager_;

		// Cursor State
		size_t cursor_ = 0;
		bool started_ = false;

		std::vector<std::string> columnNames_;
		int mode_ = 0; // 0=Rows (Default), 1=NodeStream, 2=EdgeStream
		std::string error_msg;

		void prepareMetadata() {
			// Priority: Explicit columns from Engine -> Implicit columns from Data
			const auto &executorCols = result_.getColumns();
			if (!executorCols.empty()) {
				columnNames_ = executorCols;
			} else if (!result_.getRows().empty()) {
				// Implicit columns: Extract from the first row and SORT for determinism
				for (const auto &firstRow = result_.getRows()[0]; const auto &k: firstRow | std::views::keys)
					columnNames_.push_back(k);
				std::ranges::sort(columnNames_);
			}

			// Determine mode based on content types of the first row
			// This preserves legacy "stream" behavior for single-entity queries
			if (columnNames_.size() == 1 && !result_.getRows().empty()) {
				const auto &firstVal = result_.getRows()[0].at(columnNames_[0]);
				if (firstVal.isNode()) {
					mode_ = 1; // Node Stream
				} else if (firstVal.isEdge()) {
					mode_ = 2; // Edge Stream
				}
			} else {
				mode_ = 0; // Standard Table Row
			}
		}
	};

	Result::~Result() = default;
	Result::Result() = default;
	Result::Result(Result &&) noexcept = default;
	Result &Result::operator=(Result &&) noexcept = default;

	bool Result::hasNext() const {
		if (!impl_)
			return false;
		size_t total = impl_->result_.rowCount(); // Unified: everything is in rows now

		// If we haven't started, check if there is at least 1 item
		if (!impl_->started_)
			return total > 0;

		// If we have started, check if there is a next item
		return impl_->cursor_ + 1 < total;
	}

	void Result::next() const {
		if (impl_) {
			if (!impl_->started_) {
				impl_->started_ = true;
				impl_->cursor_ = 0;
			} else {
				impl_->cursor_++;
			}
		}
	}

	Value Result::get(const std::string &key) const {
		if (!impl_ || !impl_->started_)
			return std::monostate{};

		const auto &rows = impl_->result_.getRows();
		if (impl_->cursor_ >= rows.size())
			return std::monostate{};

		const auto &row = rows[impl_->cursor_];

		// --- MODE 0: ROWS (Standard) ---
		if (impl_->mode_ == 0) {
			// 1. Exact Match
			auto it = row.find(key);
			if (it != row.end())
				return toPublicValue(it->second, impl_->dataManager_);

			// 2. Fuzzy Match (User asks "name", DB has "n.name")
			for (const auto &[dbKey, dbVal]: row) {
				if (key.find('.') == std::string::npos && dbKey.size() > key.size()) {
					if (dbKey.ends_with("." + key))
						return toPublicValue(dbVal, impl_->dataManager_);
				}
				if (dbKey.find('.') == std::string::npos && key.size() > dbKey.size()) {
					if (key.ends_with("." + dbKey))
						return toPublicValue(dbVal, impl_->dataManager_);
				}
			}
			return std::monostate{};
		}

		// --- MODE 1 & 2: ENTITY STREAM (Legacy Compatibility) ---
		// In these modes, we assume there is exactly 1 column containing the entity.
		else if (impl_->mode_ == 1 || impl_->mode_ == 2) {
			const std::string &colName = impl_->columnNames_[0];
			const auto &entityVal = row.at(colName); // Safe because mode implies !empty

			// If requesting the entity itself (key empty or standard aliases)
			if (key.empty() || key == colName) {
				return toPublicValue(entityVal, impl_->dataManager_);
			}

			// If requesting a property inside the entity
			if (entityVal.isNode()) {
				const auto &props = entityVal.asNode().getProperties();
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			} else if (entityVal.isEdge()) {
				const auto &props = entityVal.asEdge().getProperties();
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			}
		}

		return std::monostate{};
	}

	Value Result::get(int index) const {
		if (!impl_ || !impl_->started_ || index < 0 || index >= static_cast<int>(impl_->columnNames_.size()))
			return std::monostate{};

		// In the unified Row model, index maps directly to the sorted column name list
		return get(impl_->columnNames_[index]);
	}

	int Result::getColumnCount() const { return impl_ ? static_cast<int>(impl_->columnNames_.size()) : 0; }

	std::string Result::getColumnName(int index) const {
		return (impl_ && index >= 0 && index < static_cast<int>(impl_->columnNames_.size()))
					   ? impl_->columnNames_[index]
					   : "";
	}

	bool Result::isSuccess() const { return impl_ && impl_->error_msg.empty(); }

	std::string Result::getError() const {
		if (!impl_)
			return "Result not initialized";
		return impl_->error_msg;
	}

	// ========================================================================
	// 3. Database Implementation
	// ========================================================================

	class DatabaseImpl {
	public:
		explicit DatabaseImpl(const std::string &path) : db_(path) {}
		graph::Database db_;
	};

	Database::Database(const std::string &path) : impl_(std::make_unique<DatabaseImpl>(path)) {}
	Database::~Database() = default;

	void Database::open() const { impl_->db_.open(); }
	bool Database::openIfExists() const { return impl_->db_.openIfExists(); }
	void Database::close() const { impl_->db_.close(); }
	void Database::save() const {
		if (auto s = impl_->db_.getStorage())
			s->flush();
	}

	Result Database::execute(const std::string &cypher) const {
		try {
			auto internalRes = impl_->db_.getQueryEngine()->execute(cypher);
			// We MUST inject DataManager into ResultImpl to allow label resolution later
			auto dm = impl_->db_.getStorage()->getDataManager();

			Result res;
			res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes), std::move(dm));
			return res;
		} catch (const std::exception &e) {
			// Return Result in error state
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult(), nullptr); // Empty internal result
			res.impl_->error_msg = e.what();
			return res;
		} catch (...) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult(), nullptr);
			res.impl_->error_msg = "Unknown error";
			return res;
		}
	}

	void Database::createNode(const std::string &label, const std::unordered_map<std::string, Value> &props) const {
		// Use Query Engine for standard creation
		auto builder = impl_->db_.getQueryEngine()->query();
		std::unordered_map<std::string, graph::PropertyValue> internalProps;
		for (const auto &[k, v]: props)
			internalProps[k] = toInternal(v);
		auto plan = builder.create_("n", label, internalProps).build();
		(void) impl_->db_.getQueryEngine()->execute(std::move(plan));
	}

	void Database::createNodes(const std::string &label,
							   const std::vector<std::unordered_map<std::string, Value>> &propsList) const {
		if (propsList.empty())
			return;
		auto storage = impl_->db_.getStorage();
		auto dm = storage->getDataManager();

		// 1. Resolve/Create Label ID ONCE for the batch
		int64_t labelId = dm->getOrCreateLabelId(label);

		std::vector<graph::Node> nodes;
		nodes.reserve(propsList.size());

		for (const auto &props: propsList) {
			// 2. Use ID constructor
			graph::Node n(0, labelId);
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			for (const auto &[k, v]: props)
				internalProps[k] = toInternal(v);
			n.setProperties(std::move(internalProps));
			nodes.push_back(std::move(n));
		}

		dm->addNodes(nodes);
	}

	int64_t Database::createNodeRetId(const std::string &label,
									  const std::unordered_map<std::string, Value> &props) const {
		auto dm = impl_->db_.getStorage()->getDataManager();

		// 1. Resolve/Create Label ID
		int64_t labelId = dm->getOrCreateLabelId(label);

		// 2. Use ID constructor
		graph::Node node(0, labelId);
		dm->addNode(node);
		int64_t newId = node.getId();

		if (!props.empty()) {
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			for (const auto &[k, v]: props)
				internalProps[k] = toInternal(v);
			dm->addNodeProperties(newId, internalProps);
		}
		return newId;
	}

	void Database::createEdgeById(int64_t sourceId, int64_t targetId, const std::string &edgeLabel,
								  const std::unordered_map<std::string, Value> &props) const {
		auto dm = impl_->db_.getStorage()->getDataManager();

		// 1. Resolve/Create Label ID
		int64_t labelId = dm->getOrCreateLabelId(edgeLabel);

		// 2. Use ID constructor
		graph::Edge edge(0, sourceId, targetId, labelId);
		dm->addEdge(edge);

		if (!props.empty()) {
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			for (const auto &[k, v]: props)
				internalProps[k] = toInternal(v);
			dm->addEdgeProperties(edge.getId(), internalProps);
		}
	}

	void Database::createEdge(const std::string &sourceLabel, const std::string &sourceKey, const Value &sourceVal,
							  const std::string &targetLabel, const std::string &targetKey, const Value &targetVal,
							  const std::string &edgeLabel, const std::unordered_map<std::string, Value> &props) const {
		// This uses Query Engine, which internally handles label resolution if implemented correctly there.
		// Since QueryEngine likely calls DataManager/Storage eventually, or builds a Plan,
		// we just delegate to it.
		auto builder = impl_->db_.getQueryEngine()->query();
		std::unordered_map<std::string, graph::PropertyValue> edgeProps;
		for (const auto &[k, v]: props)
			edgeProps[k] = toInternal(v);

		// Ensure variables match create_ arguments
		builder.match_("a", sourceLabel, sourceKey, toInternal(sourceVal));
		builder.match_("b", targetLabel, targetKey, toInternal(targetVal));
		builder.create_("e", edgeLabel, "a", "b", edgeProps);
		builder.return_({"e"});

		auto plan = builder.build();
		(void) impl_->db_.getQueryEngine()->execute(std::move(plan));
	}

	std::vector<Node> Database::getShortestPath(int64_t startId, int64_t endId, int maxDepth) const {
		try {
			auto storage = impl_->db_.getStorage();
			if (!storage)
				return {};
			auto dm = storage->getDataManager();

			graph::query::algorithm::GraphAlgorithm algo(dm);
			// Default "out" for directed shortest path
			auto internalPath = algo.findShortestPath(startId, endId, "out", maxDepth);

			std::vector<Node> publicPath;
			publicPath.reserve(internalPath.size());
			for (const auto &n: internalPath) {
				// Fetch full properties
				graph::Node fullNode = dm->getNode(n.getId());
				fullNode.setProperties(dm->getNodeProperties(n.getId()));
				// Pass DM for label resolution
				publicPath.push_back(toPublicNode(fullNode, dm));
			}
			return publicPath;
		} catch (...) {
			return {};
		}
	}

	void Database::bfs(int64_t startNodeId, const std::function<bool(const Node &)> &visitor) const {
		try {
			auto storage = impl_->db_.getStorage();
			if (!storage)
				return;
			auto dm = storage->getDataManager();

			graph::query::algorithm::GraphAlgorithm algo(dm);
			auto internalVisitor = [&](int64_t nodeId, [[maybe_unused]] int depth) -> bool {
				graph::Node internalNode = dm->getNode(nodeId);
				internalNode.setProperties(dm->getNodeProperties(nodeId));
				// Pass DM for label resolution
				return visitor(toPublicNode(internalNode, dm));
			};
			algo.breadthFirstTraversal(startNodeId, internalVisitor, "out");
		} catch (...) {
		}
	}

} // namespace metrix
