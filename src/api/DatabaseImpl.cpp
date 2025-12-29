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
#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include "graph/query/api/QueryBuilder.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "metrix/metrix.hpp"

namespace metrix {

    // --- Helpers: Type Conversion ---

    graph::PropertyValue toInternal(const Value& v) {
        return std::visit([](auto&& arg) -> graph::PropertyValue {
            return graph::PropertyValue(arg);
        }, v);
    }

    // Explicit helper for PropertyValue -> Value variant conversion
    Value toPublicValue(const graph::PropertyValue& v) {
        return std::visit([](auto&& arg) -> Value {
            return arg;
        }, v.getVariant());
    }

    Value toPublic(const graph::PropertyValue& v) {
        return v.toString(); // Fallback for string representation if needed
    }

    Node toPublicNode(const graph::Node& internalNode) {
        Node pubNode;
        pubNode.id = internalNode.getId();
        pubNode.label = internalNode.getLabel();
        for (const auto& [key, val] : internalNode.getProperties()) {
            pubNode.properties[key] = toPublicValue(val);
        }
        return pubNode;
    }

    // --- ResultImpl ---

    class ResultImpl {
    public:
        explicit ResultImpl(graph::query::QueryResult internalRes)
            : result_(std::move(internalRes)) {}
        graph::query::QueryResult result_;
        size_t cursor_ = 0;
    };

    Result::~Result() = default;
    Result::Result() = default;
    Result::Result(Result&&) noexcept = default;
    Result& Result::operator=(Result&&) noexcept = default;

    bool Result::hasNext() {
        return impl_ && impl_->cursor_ < impl_->result_.rowCount();
    }

    void Result::next() {
        if (impl_) impl_->cursor_++;
    }

    Value Result::get(const std::string& key) const {
        if (!impl_) return std::monostate{};
        const auto& rows = impl_->result_.getRows();
        if (impl_->cursor_ >= rows.size()) return std::monostate{};

        const auto& row = rows[impl_->cursor_];
        auto it = row.find(key);
        if (it != row.end()) return toPublicValue(it->second);
        return std::monostate{};
    }

    bool Result::isSuccess() const { return true; }
    std::string Result::getError() const { return ""; }

    // --- DatabaseImpl Definition ---

    class DatabaseImpl {
    public:
        DatabaseImpl(const std::string& path) : db_(path) {}
        graph::Database db_;
    };

    // --- Database Method Implementations ---

    Database::Database(const std::string& path)
        : impl_(std::make_unique<DatabaseImpl>(path)) {}

    Database::~Database() = default;

    void Database::open() { impl_->db_.open(); }
    void Database::close() { impl_->db_.close(); }

    void Database::save() {
        if (auto storage = impl_->db_.getStorage()) {
            storage->flush();
        }
    }

    Result Database::execute(const std::string& cypher) {
        try {
            auto internalRes = impl_->db_.getQueryEngine()->execute(cypher);
            Result res;
            res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes));
            return res;
        } catch (...) {
            return Result();
        }
    }

    // Single Node Creation (via Query Pipeline)
    void Database::createNode(const std::string& label, const std::unordered_map<std::string, Value>& props) {
        auto builder = impl_->db_.getQueryEngine()->query();
        std::unordered_map<std::string, graph::PropertyValue> internalProps;
        for(const auto& [k, v] : props) internalProps[k] = toInternal(v);

        auto plan = builder.create_("n", label, internalProps).build();
        impl_->db_.getQueryEngine()->execute(std::move(plan));
    }

    // Batch Node Creation (Direct Storage Access)
    // Essential for NativeBatchInsertBench
    void Database::createNodes(const std::string& label, const std::vector<std::unordered_map<std::string, Value>>& propsList) {
        if (propsList.empty()) return;

        auto storage = impl_->db_.getStorage();
        if (!storage) return;

        std::vector<graph::Node> nodes;
        nodes.reserve(propsList.size());

        for (const auto& props : propsList) {
            graph::Node n(0, label);
            std::unordered_map<std::string, graph::PropertyValue> internalProps;
            for (const auto& [k, v] : props) {
                internalProps[k] = toInternal(v);
            }
            // Use setProperties helper to hydrate the object before insertion
            n.setProperties(std::move(internalProps));
            nodes.push_back(std::move(n));
        }

        // Call DataManager::addNodes (Batch API)
        storage->getDataManager()->addNodes(nodes);
    }

    // Create Edge by Logic (MATCH -> CREATE)
    void Database::createEdge(
        const std::string& sourceLabel, const std::string& sourceKey, const Value& sourceVal,
        const std::string& targetLabel, const std::string& targetKey, const Value& targetVal,
        const std::string& edgeLabel, const std::unordered_map<std::string, Value>& props)
    {
        auto builder = impl_->db_.getQueryEngine()->query();

        std::unordered_map<std::string, graph::PropertyValue> edgeProps;
        for(const auto& [k, v] : props) edgeProps[k] = toInternal(v);

        // 1. Match Source: MATCH (a:Src {key: val})
        builder.match_("a", sourceLabel, sourceKey, toInternal(sourceVal));

        // 2. Match Target: MATCH (b:Tgt {key: val})
        // Note: The builder will chain this, creating a CartesianProduct internally in the Visitor/Planner logic
        // if using the parser, but here we construct raw operators.
        // Assuming QueryBuilder handles chaining correctly (as implemented).
        builder.match_("b", targetLabel, targetKey, toInternal(targetVal));

        // 3. Create Edge: CREATE (a)-[r:Type {props}]->(b)
        builder.create_("e", edgeLabel, "a", "b", edgeProps);

        auto plan = builder.build();
        impl_->db_.getQueryEngine()->execute(std::move(plan));
    }

    // Direct Node Creation (Returns ID)
    int64_t Database::createNodeRetId(const std::string& label, const std::unordered_map<std::string, Value>& props) {
        auto storage = impl_->db_.getStorage();
        auto dm = storage->getDataManager();

        graph::Node node(0, label);
        // Persist Node
        dm->addNode(node);
        int64_t newId = node.getId();

        // Persist Properties
        if (!props.empty()) {
            std::unordered_map<std::string, graph::PropertyValue> internalProps;
            for (const auto& [k, v] : props) internalProps[k] = toInternal(v);
            dm->addNodeProperties(newId, internalProps);
        }
        return newId;
    }

    // Direct Edge Creation (By ID)
    void Database::createEdgeById(int64_t sourceId, int64_t targetId,
                                  const std::string& edgeLabel,
                                  const std::unordered_map<std::string, Value>& props) {
        auto storage = impl_->db_.getStorage();
        auto dm = storage->getDataManager();

        graph::Edge edge(0, sourceId, targetId, edgeLabel);
        dm->addEdge(edge);

        if (!props.empty()) {
            std::unordered_map<std::string, graph::PropertyValue> internalProps;
            for (const auto& [k, v] : props) internalProps[k] = toInternal(v);
            dm->addEdgeProperties(edge.getId(), internalProps);
        }
    }

    // Algorithms
    std::vector<Node> Database::getShortestPath(int64_t startId, int64_t endId, int maxDepth) {
        try {
            auto storage = impl_->db_.getStorage();
            if (!storage) return {};
            auto dm = storage->getDataManager();

            graph::query::algorithm::GraphAlgorithm algo(dm);

            // Call internal algo
            auto internalPath = algo.findShortestPath(startId, endId, "both", maxDepth);

            std::vector<Node> publicPath;
            publicPath.reserve(internalPath.size());
            for (const auto& n : internalPath) {
                publicPath.push_back(toPublicNode(n));
            }
            return publicPath;
        } catch (...) {
            return {};
        }
    }

    void Database::bfs(int64_t startNodeId, std::function<bool(const Node&)> visitor) {
        try {
            auto storage = impl_->db_.getStorage();
            if (!storage) return;
            auto dm = storage->getDataManager();

            graph::query::algorithm::GraphAlgorithm algo(dm);

            auto internalVisitor = [&](int64_t nodeId, [[maybe_unused]] int depth) -> bool {
                // 1. Fetch Header
                graph::Node internalNode = dm->getNode(nodeId);
                // 2. Hydrate
                internalNode.setProperties(dm->getNodeProperties(nodeId));
                // 3. Convert
                Node publicNode = toPublicNode(internalNode);
                // 4. Callback
                return visitor(publicNode);
            };

            algo.breadthFirstTraversal(startNodeId, internalVisitor, "both");

        } catch (...) {
            // Log error
        }
    }

} // namespace metrix