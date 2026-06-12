#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Types.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	class PropertyPredicateScanKernel {
	public:
		PropertyPredicateScanKernel(std::shared_ptr<storage::DataManager> dm,
		                            std::vector<VectorizedPropertyPredicate> predicates,
		                            concurrent::ThreadPool *threadPool = nullptr);

		[[nodiscard]] static PropertyPredicateScanKernel
		fromEqualityPredicates(std::shared_ptr<storage::DataManager> dm,
		                       const std::unordered_map<std::string, PropertyValue> &predicates,
		                       concurrent::ThreadPool *threadPool = nullptr);

		[[nodiscard]] bool empty() const noexcept { return predicateKernel_.empty(); }
		[[nodiscard]] bool matchesMap(const std::unordered_map<std::string, PropertyValue> &properties) const;

		[[nodiscard]] size_t countPropertyEntities(const std::vector<int64_t> &propertyEntityIds) const;
		[[nodiscard]] storage::PropertyEntityPredicateCountResult
		countPropertyEntityMatches(const std::vector<int64_t> &propertyEntityIds) const;
		[[nodiscard]] std::optional<storage::PropertyEntityPredicateCountResult>
		countOwnerTypeMatches(EntityType ownerType) const;
		[[nodiscard]] std::optional<storage::PropertyEntityPredicateCountResult>
		countAllOwnerProperties(EntityType ownerType) const;
		[[nodiscard]] std::optional<std::vector<int64_t>>
		collectAllOwnerIds(EntityType ownerType,
						   const storage::PropertyEntityOwnerPredicateScanOptions &options = {}) const;

		[[nodiscard]] storage::PropertyEntityPredicateMatchResult matchPropertyEntities(
				const std::vector<int64_t> &propertyEntityIds,
				const std::vector<size_t> &rows,
				size_t rowCount,
				storage::PropertyEntityPredicateMatchOptions options = {}) const;

	private:
		PropertyPredicateScanKernel(std::shared_ptr<storage::DataManager> dm,
		                            PropertyPredicateKernel predicateKernel,
		                            concurrent::ThreadPool *threadPool);

		void compileStoragePredicates();

		std::shared_ptr<storage::DataManager> dm_;
		PropertyPredicateKernel predicateKernel_;
		concurrent::ThreadPool *threadPool_ = nullptr;
		bool useEqualityMatcher_ = false;
		std::unordered_map<std::string, PropertyValue> equalityPredicates_;
		std::vector<storage::PropertyEntityPredicate> storagePredicates_;
	};

} // namespace graph::query::execution
