#pragma once

#include <memory>
#include <string>
#include <vector>

#include "graph/query/execution/RelationshipExpandConfig.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution {

	struct RelationshipCandidateSet {
		std::vector<int64_t> ids;
		std::vector<std::string> propertyKeysSatisfied;
		bool available = false;
		bool activeOnly = false;
		bool typeSatisfied = false;
	};

	class RelationshipCandidateSource {
	public:
		RelationshipCandidateSource(std::shared_ptr<storage::DataManager> dm,
		                            std::shared_ptr<indexes::IndexManager> im);

		[[nodiscard]] RelationshipCandidateSet collect(const DirectRelationshipCountConfig &config) const;

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
	};

} // namespace graph::query::execution
