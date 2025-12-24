/**
 * @file DatabaseImpl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/16
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "metrix/metrix.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Property.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/api/QueryBuilder.hpp"
#include "graph/query/api/QueryEngine.hpp"

namespace metrix {

    // --- Helper: Type Conversion ---

    // Convert public API Value to internal PropertyValue
    graph::PropertyValue toInternal(const Value& v) {
        return std::visit([](auto&& arg) -> graph::PropertyValue {
            return graph::PropertyValue(arg);
        }, v);
    }

    // Convert internal PropertyValue to public API Value
    Value toPublic(const graph::PropertyValue& v) {
        // Simplified conversion. In a full implementation, you should check
        // the type of PropertyValue and convert precisely.
        return v.toString();
    }

    // --- ResultImpl Definition ---

    class ResultImpl {
    public:
        explicit ResultImpl(graph::query::QueryResult internalRes)
            : result_(std::move(internalRes)) {}

        graph::query::QueryResult result_;
        size_t cursor_ = 0;
    };

    // --- Result Method Implementations ---

    // Destructor must be here where ResultImpl is visible
    Result::~Result() = default;

    // Default constructor
    Result::Result() = default;

    // Move constructor
    Result::Result(Result&&) noexcept = default;
    Result& Result::operator=(Result&&) noexcept = default;

    bool Result::hasNext() {
        if (!impl_) return false;
        // Check if cursor is within bounds of rows
        return impl_->cursor_ < impl_->result_.rowCount();
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
        if (it != row.end()) {
            return toPublic(it->second);
        }
        return std::monostate{};
    }

    bool Result::isSuccess() const { return true; } // Placeholder
    std::string Result::getError() const { return ""; } // Placeholder

    // --- DatabaseImpl Definition ---

    class DatabaseImpl {
    public:
        DatabaseImpl(const std::string& path, graph::storage::OpenMode mode)
            : db_(path, mode) {}

        // The internal engine instance
        graph::Database db_;
    };

    // --- Database Method Implementations ---

	Database::Database(const std::string& path)
		: impl_(std::make_unique<DatabaseImpl>(path, graph::storage::OpenMode::CREATE_OR_OPEN)) {}

    // [CRITICAL] The destructor is implemented HERE, where DatabaseImpl is fully defined.
    // This allows unique_ptr to correctly call delete on DatabaseImpl.
    Database::~Database() = default;

    void Database::open() {
        impl_->db_.open();
    }

    void Database::close() {
        impl_->db_.close();
    }

    void Database::save() {
        if (auto storage = impl_->db_.getStorage()) {
            storage->flush();
        }
    }

    Result Database::execute(const std::string& cypher) {
        try {
            // 1. Execute query using internal engine
            auto internalRes = impl_->db_.getQueryEngine()->execute(cypher);

            // 2. Wrap result in Pimpl class
            Result res;
            res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes));
            return res;
        } catch (...) {
            // In case of error, return an empty/error result
            // (Proper error handling should capture the message)
            return Result();
        }
    }

    void Database::createNode(const std::string& label, const std::unordered_map<std::string, Value>& props) {
        // 1. Get the QueryBuilder from the internal engine
        auto builder = impl_->db_.getQueryEngine()->query();

        // 2. Convert properties
        std::unordered_map<std::string, graph::PropertyValue> internalProps;
        for(const auto& [k, v] : props) {
            internalProps[k] = toInternal(v);
        }

        // 3. Build the Physical Operator Plan (skip parsing)
        auto plan = builder.create_("n", label, internalProps).build();

        // 4. Execute the plan
        // Note: ensure QueryEngine::execute(unique_ptr<PhysicalOperator>) is accessible
        impl_->db_.getQueryEngine()->execute(std::move(plan));
    }

	void Database::createEdge(
		const std::string& sourceLabel, const std::string& sourceKey, const Value& sourceVal,
		const std::string& targetLabel, const std::string& targetKey, const Value& targetVal,
		const std::string& edgeLabel, const std::unordered_map<std::string, Value>& props)
    {
    	// Bypass Parser!
    	// Logic: MATCH (a:Src {k:v}), (b:Tgt {k:v}) CREATE (a)-[r:Type {props}]->(b)

    	auto builder = impl_->db_.getQueryEngine()->query();

    	std::unordered_map<std::string, graph::PropertyValue> edgeProps;
    	for(const auto& [k, v] : props) edgeProps[k] = toInternal(v);

    	// 1. Match Source
    	builder.match_("a", sourceLabel).where_("a", sourceKey, toInternal(sourceVal));

    	// 2. Match Target (This requires QueryBuilder to support multiple matches or we define a specialized operator)
    	// Note: Your current QueryBuilder might need extending to support chaining Matches for creating edges efficiently.
    	// For now, let's assume a simplified creation or implementation inside internal engine.

    	// ... (Implementation details depend on QueryBuilder capabilities)
    }

	int64_t Database::createNodeRetId(const std::string& label, const std::unordered_map<std::string, Value>& props) {
        auto storage = impl_->db_.getStorage();
        auto dm = storage->getDataManager();

        // 1. Create Node Object (ID 0 tells Manager to allocate)
        graph::Node node(0, label);

        // 2. Persist Node (Allocates ID, triggers OnNodeAdded observer)
        dm->addNode(node);
        int64_t newId = node.getId();

        // 3. Add Properties (Triggers OnNodeUpdated observer)
        if (!props.empty()) {
            std::unordered_map<std::string, graph::PropertyValue> internalProps;
            for (const auto& [k, v] : props) {
                internalProps[k] = toInternal(v);
            }
            dm->addNodeProperties(newId, internalProps);
        }

        return newId;
    }

    void Database::createEdgeById(int64_t sourceId, int64_t targetId,
                                  const std::string& edgeLabel,
                                  const std::unordered_map<std::string, Value>& props) {
        auto storage = impl_->db_.getStorage();
        auto dm = storage->getDataManager();

        // 1. Validation (Optional, but good for debugging tools)
        // Ensure nodes actually exist to avoid corrupt graph topology
        // Note: getNode() might throw or return empty if invalid.
        // For max speed, you might skip this check if you trust your tool logic.
        /*
        auto src = dm->getNode(sourceId);
        auto tgt = dm->getNode(targetId);
        if (src.getId() == 0 || tgt.getId() == 0) {
            throw std::runtime_error("Invalid Node IDs passed to createEdgeById");
        }
        */

        // 2. Create Edge Object
        // ID 0 -> Auto allocate
        graph::Edge edge(0, sourceId, targetId, edgeLabel);

        // 3. Persist Edge (Triggers OnEdgeAdded)
        // NOTE: Ensure DataManager::addEdge calls linkEdge internally!
        // Based on our previous fix, it does.
        dm->addEdge(edge);

        // 4. Add Properties
        if (!props.empty()) {
            std::unordered_map<std::string, graph::PropertyValue> internalProps;
            for (const auto& [k, v] : props) {
                internalProps[k] = toInternal(v);
            }
            dm->addEdgeProperties(edge.getId(), internalProps);
        }
    }

} // namespace metrix