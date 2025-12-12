/**
 * @file QueryEngine.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/api/QueryEngine.hpp"
#include "CypherParserImpl.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query {

	QueryEngine::QueryEngine(std::shared_ptr<storage::FileStorage> storage)
		: storage_(std::move(storage)) {

		auto dm = storage_->getDataManager();
		indexManager_ = std::make_shared<indexes::IndexManager>(storage_);
		indexManager_->initialize();
		dm->registerObserver(indexManager_);

		// Note: queryPlanner_ is now shared_ptr for the Builder
		queryPlanner_ = std::make_shared<QueryPlanner>(dm, indexManager_);
		queryExecutor_ = std::make_unique<QueryExecutor>();
	}

	QueryEngine::~QueryEngine() = default;

	std::shared_ptr<parser::IQueryParser> QueryEngine::getParser(Language lang) {
		if (parsers_.find(lang) == parsers_.end()) {
			if (lang == Language::Cypher) {
				// Pass the planner to the parser so it can build trees
				parsers_[lang] = std::make_shared<parser::cypher::CypherParserImpl>(queryPlanner_);
			} else {
				throw std::runtime_error("Unsupported query language.");
			}
		}
		return parsers_[lang];
	}

	QueryResult QueryEngine::execute(const std::string& queryStr, Language lang) {
		// 1. Parse into Operator Tree
		auto planTree = getParser(lang)->parse(queryStr);
		// 2. Execute
		return queryExecutor_->execute(std::move(planTree));
	}

    // --- Index Management Wrappers (Unchanged Logic) ---
    bool QueryEngine::buildIndexes(const std::string &entityType) const {
        if (entityType != "node" && entityType != "edge") return false;
        return indexManager_->buildIndexes(entityType);
    }

    bool QueryEngine::buildPropertyIndex(const std::string &entityType, const std::string &key) const {
        if (entityType != "node" && entityType != "edge") return false;
        return indexManager_->buildPropertyIndex(entityType, key);
    }

    bool QueryEngine::dropIndex(const std::string &entityType, const std::string &indexType, const std::string &key) const {
        if (entityType != "node" && entityType != "edge") return false;
        return indexManager_->dropIndex(entityType, indexType, key);
    }

    std::vector<std::pair<std::string, std::string>> QueryEngine::listIndexes(const std::string &entityType) const {
        if (entityType != "node" && entityType != "edge") return {};
        return indexManager_->listIndexes(entityType);
    }

} // namespace graph::query