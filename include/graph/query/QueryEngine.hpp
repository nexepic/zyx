/**
 * @file QueryEngine.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <memory>
#include "QueryExecutor.hpp"
#include "QueryPlanner.hpp"
#include "QueryBuilder.hpp"
#include "parser/common/IQueryParser.hpp"

namespace graph::query {

	enum class Language { Cypher };

	class QueryEngine {
	public:
		explicit QueryEngine(std::shared_ptr<storage::FileStorage> storage);
		~QueryEngine();

		// --- Main Execution ---
		QueryResult execute(const std::string& query, Language lang = Language::Cypher);

		// Returns a builder to construct queries programmatically
		QueryBuilder query() const {
			return QueryBuilder(queryPlanner_);
		}

		// --- Index Management (Keep these) ---
		bool buildIndexes(const std::string &entityType) const;
		bool buildPropertyIndex(const std::string &entityType, const std::string &key) const;
		bool dropIndex(const std::string &entityType, const std::string &indexType, const std::string &key = "") const;
		std::vector<std::pair<std::string, std::string>> listIndexes(const std::string &entityType) const;

		[[nodiscard]] std::shared_ptr<indexes::IndexManager> getIndexManager() const { return indexManager_; }

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::shared_ptr<QueryPlanner> queryPlanner_; // Shared so Builder can use it
		std::unique_ptr<QueryExecutor> queryExecutor_;

		std::unordered_map<Language, std::shared_ptr<parser::IQueryParser>> parsers_;
		std::shared_ptr<parser::IQueryParser> getParser(Language lang);
	};

}