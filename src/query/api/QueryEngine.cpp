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
		if (!parsers_.contains(lang)) {
			if (lang == Language::Cypher) {
				// Pass the planner to the parser so it can build trees
				parsers_[lang] = std::make_shared<parser::cypher::CypherParserImpl>(queryPlanner_);
			} else {
				throw std::runtime_error("Unsupported query language.");
			}
		}
		return parsers_[lang];
	}

	QueryResult QueryEngine::execute(const std::string& query, const Language lang) {
		// 1. Parse into Operator Tree
		auto planTree = getParser(lang)->parse(query);
		// 2. Execute
		return queryExecutor_->execute(std::move(planTree));
	}

	QueryResult QueryEngine::execute(std::unique_ptr<execution::PhysicalOperator> plan) const {
		return queryExecutor_->execute(std::move(plan));
	}

} // namespace graph::query