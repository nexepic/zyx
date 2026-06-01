#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	struct RelationshipColumnarCountRequest {
		int64_t beginId = 1;
		int64_t endId = 0;
		int64_t typeId = 0;
		std::unordered_map<std::string, PropertyValue> propertyPredicates;
		std::vector<VectorizedPropertyPredicate> vectorPredicates;
	};

	struct RelationshipColumnarCountResult {
		int64_t count = 0;
		size_t propertyCandidates = 0;
		size_t fallbackEdges = 0;
	};

	class RelationshipColumnarCountKernel {
	public:
		explicit RelationshipColumnarCountKernel(
			std::shared_ptr<storage::DataManager> dm,
			concurrent::ThreadPool *pool = nullptr);

		[[nodiscard]] std::optional<RelationshipColumnarCountResult>
		count(const RelationshipColumnarCountRequest &request) const;

	private:
		[[nodiscard]] bool propertyMapMatches(
			const std::unordered_map<std::string, PropertyValue> &properties,
			const PropertyPredicateKernel &predicateKernel) const;

		std::shared_ptr<storage::DataManager> dm_;
		concurrent::ThreadPool *pool_ = nullptr;
	};

} // namespace graph::query::execution
