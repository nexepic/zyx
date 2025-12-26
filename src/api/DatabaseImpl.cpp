/**
 * @file DatabaseImpl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/16
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Database.hpp"
#include "graph/core/Property.hpp"
#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include "graph/query/api/QueryBuilder.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "metrix/metrix.hpp"

namespace metrix {

	// --- Helpers: Type Conversion ---

	graph::PropertyValue toInternal(const Value &v) {
		return std::visit([](auto &&arg) -> graph::PropertyValue { return graph::PropertyValue(arg); }, v);
	}

	Value toPublic(const graph::PropertyValue &v) {
		// Use variant internal visit to extract raw value
		// Note: Make sure PropertyValue has a way to expose raw data or visit it.
		// Assuming toPublicValue helper logic is integrated here or PropertyValue exposes getter.
		// For now using toString based on your previous code, but variant copy is better:
		return v.toString();
	}

	// Explicit helper for PropertyValue -> Value variant conversion
	Value toPublicValue(const graph::PropertyValue &v) {
		return std::visit([](auto &&arg) -> Value { return arg; }, v.getVariant());
	}

	Node toPublicNode(const graph::Node &internalNode) {
		Node pubNode;
		pubNode.id = internalNode.getId();
		pubNode.label = internalNode.getLabel();
		for (const auto &[key, val]: internalNode.getProperties()) {
			pubNode.properties[key] = toPublicValue(val);
		}
		return pubNode;
	}

	// --- ResultImpl ---

	class ResultImpl {
	public:
		explicit ResultImpl(graph::query::QueryResult internalRes) : result_(std::move(internalRes)) {}
		graph::query::QueryResult result_;
		size_t cursor_ = 0;
	};

	Result::~Result() = default;
	Result::Result() = default;
	Result::Result(Result &&) noexcept = default;
	Result &Result::operator=(Result &&) noexcept = default;

	bool Result::hasNext() { return impl_ && impl_->cursor_ < impl_->result_.rowCount(); }

	void Result::next() {
		if (impl_)
			impl_->cursor_++;
	}

	Value Result::get(const std::string &key) const {
		if (!impl_)
			return std::monostate{};
		const auto &rows = impl_->result_.getRows();
		if (impl_->cursor_ >= rows.size())
			return std::monostate{};

		const auto &row = rows[impl_->cursor_];
		auto it = row.find(key);
		if (it != row.end())
			return toPublicValue(it->second);
		return std::monostate{};
	}

	bool Result::isSuccess() const { return true; }
	std::string Result::getError() const { return ""; }

	// --- DatabaseImpl Definition ---

	class DatabaseImpl {
	public:
		DatabaseImpl(const std::string &path) : db_(path) {}

		graph::Database db_;
	};

	// --- Database Method Implementations ---

	Database::Database(const std::string &path) : impl_(std::make_unique<DatabaseImpl>(path)) {}

	Database::~Database() = default;

	void Database::open() { impl_->db_.open(); }
	void Database::close() { impl_->db_.close(); }

	void Database::save() {
		if (auto storage = impl_->db_.getStorage()) {
			storage->flush();
		}
	}

	Result Database::execute(const std::string &cypher) {
		try {
			auto internalRes = impl_->db_.getQueryEngine()->execute(cypher);
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes));
			return res;
		} catch (...) {
			return Result();
		}
	}

	void Database::createNode(const std::string &label, const std::unordered_map<std::string, Value> &props) {
		auto builder = impl_->db_.getQueryEngine()->query();
		std::unordered_map<std::string, graph::PropertyValue> internalProps;
		for (const auto &[k, v]: props)
			internalProps[k] = toInternal(v);

		auto plan = builder.create_("n", label, internalProps).build();
		impl_->db_.getQueryEngine()->execute(std::move(plan));
	}

	int64_t Database::createNodeRetId(const std::string &label, const std::unordered_map<std::string, Value> &props) {
		auto storage = impl_->db_.getStorage();
		auto dm = storage->getDataManager();

		graph::Node node(0, label);
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
								  const std::unordered_map<std::string, Value> &props) {
		auto storage = impl_->db_.getStorage();
		auto dm = storage->getDataManager();

		graph::Edge edge(0, sourceId, targetId, edgeLabel);
		dm->addEdge(edge); // addEdge handles linking

		if (!props.empty()) {
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			for (const auto &[k, v]: props)
				internalProps[k] = toInternal(v);
			dm->addEdgeProperties(edge.getId(), internalProps);
		}
	}

	std::vector<Node> Database::getShortestPath(int64_t startId, int64_t endId, int maxDepth) {
		try {
			auto storage = impl_->db_.getStorage();
			if (!storage)
				return {};
			auto dm = storage->getDataManager();

			graph::query::algorithm::GraphAlgorithm algo(dm);
			// Internal API: (start, end, direction, maxDepth)
			auto internalPath = algo.findShortestPath(startId, endId, "both", maxDepth);

			std::vector<Node> publicPath;
			publicPath.reserve(internalPath.size());
			for (const auto &n: internalPath) {
				publicPath.push_back(toPublicNode(n));
			}
			return publicPath;
		} catch (...) {
			return {};
		}
	}

	// BFS Implementation
	void Database::bfs(int64_t startNodeId, std::function<bool(const Node &)> visitor) {
		try {
			auto storage = impl_->db_.getStorage();
			if (!storage)
				return;
			auto dm = storage->getDataManager();

			graph::query::algorithm::GraphAlgorithm algo(dm);

			// Bridge Internal ID-based callback -> Public Node-based callback
			auto internalVisitor = [&](int64_t nodeId, int depth) -> bool {
				// 1. Fetch Node Header
				graph::Node internalNode = dm->getNode(nodeId);

				// 2. Hydrate Properties (Lazy Load)
				// We load properties because the public API expects a full Node object
				internalNode.setProperties(dm->getNodeProperties(nodeId));

				// 3. Convert to Public
				Node publicNode = toPublicNode(internalNode);

				// 4. Invoke User Callback
				return visitor(publicNode);
			};

			// Call internal algorithm
			algo.breadthFirstTraversal(startNodeId, internalVisitor, "both");

		} catch (...) {
			// Log error
		}
	}

} // namespace metrix
