#include "graph/query/execution/PropertyPredicateScanKernel.hpp"

#include <utility>

namespace graph::query::execution {

	PropertyPredicateScanKernel::PropertyPredicateScanKernel(
			std::shared_ptr<storage::DataManager> dm,
			std::vector<VectorizedPropertyPredicate> predicates,
			concurrent::ThreadPool *threadPool) :
		PropertyPredicateScanKernel(std::move(dm), PropertyPredicateKernel(std::move(predicates)), threadPool) {}

	PropertyPredicateScanKernel PropertyPredicateScanKernel::fromEqualityPredicates(
			std::shared_ptr<storage::DataManager> dm,
			const std::unordered_map<std::string, PropertyValue> &predicates,
			concurrent::ThreadPool *threadPool) {
		return PropertyPredicateScanKernel(
				std::move(dm), PropertyPredicateKernel::fromEqualityPredicates(predicates), threadPool);
	}

	PropertyPredicateScanKernel::PropertyPredicateScanKernel(
			std::shared_ptr<storage::DataManager> dm,
			PropertyPredicateKernel predicateKernel,
			concurrent::ThreadPool *threadPool) :
		dm_(std::move(dm)), predicateKernel_(std::move(predicateKernel)), threadPool_(threadPool) {
		compileStoragePredicates();
	}

	void PropertyPredicateScanKernel::compileStoragePredicates() {
		if (predicateKernel_.empty()) {
			return;
		}
		storagePredicates_ = predicateKernel_.toStoragePredicates();
		useEqualityMatcher_ = predicateKernel_.containsOnlyEqualityPredicates();
		if (useEqualityMatcher_) {
			equalityPredicates_ = predicateKernel_.toEqualityPredicates();
		}
	}

	bool PropertyPredicateScanKernel::matchesMap(
			const std::unordered_map<std::string, PropertyValue> &properties) const {
		return predicateKernel_.matchesMap(properties);
	}

	size_t PropertyPredicateScanKernel::countPropertyEntities(
			const std::vector<int64_t> &propertyEntityIds) const {
		return countPropertyEntityMatches(propertyEntityIds).matchedCount;
	}

	storage::PropertyEntityPredicateCountResult PropertyPredicateScanKernel::countPropertyEntityMatches(
			const std::vector<int64_t> &propertyEntityIds) const {
		if (!dm_ || propertyEntityIds.empty() || predicateKernel_.empty()) {
			return {};
		}
		return useEqualityMatcher_
					   ? dm_->bulkCountPropertyEntityPredicateMatches(propertyEntityIds, equalityPredicates_, threadPool_)
					   : dm_->bulkCountPropertyEntityPredicateSpecMatches(propertyEntityIds, storagePredicates_, threadPool_);
	}

	std::optional<storage::PropertyEntityPredicateCountResult>
	PropertyPredicateScanKernel::countOwnerTypeMatches(EntityType ownerType) const {
		if (!dm_ || predicateKernel_.empty() ||
			!dm_->canCountPropertyEntityPredicatesByOwnerType(ownerType)) {
			return std::nullopt;
		}
		return dm_->bulkCountPropertyEntityPredicateSpecsByOwnerType(ownerType, storagePredicates_, threadPool_);
	}

	std::optional<storage::PropertyEntityPredicateCountResult>
	PropertyPredicateScanKernel::countAllOwnerProperties(EntityType ownerType) const {
		if (!dm_ || predicateKernel_.empty() ||
			!dm_->canCountAllPropertyPredicatesByOwnerType(ownerType)) {
			return std::nullopt;
		}
		return dm_->bulkCountAllPropertyPredicateSpecsByOwnerType(ownerType, storagePredicates_, threadPool_);
	}

	std::optional<std::vector<int64_t>> PropertyPredicateScanKernel::collectAllOwnerIds(
			EntityType ownerType,
			const storage::PropertyEntityOwnerPredicateScanOptions &options) const {
		if (!dm_ || predicateKernel_.empty() ||
			!dm_->canCountAllPropertyPredicatesByOwnerType(ownerType)) {
			return std::nullopt;
		}
		return dm_->bulkCollectAllPropertyPredicateOwnerIdsByOwnerType(
				ownerType, storagePredicates_, options, threadPool_);
	}

	storage::PropertyEntityPredicateMatchResult PropertyPredicateScanKernel::matchPropertyEntities(
			const std::vector<int64_t> &propertyEntityIds,
			const std::vector<size_t> &rows,
			size_t rowCount,
			storage::PropertyEntityPredicateMatchOptions options) const {
		if (!dm_ || propertyEntityIds.empty() || predicateKernel_.empty()) {
			return {};
		}
		return useEqualityMatcher_
					   ? dm_->bulkMatchPropertyEntityPredicates(
								 propertyEntityIds, rows, rowCount, equalityPredicates_, threadPool_, options)
					   : dm_->bulkMatchPropertyEntityPredicateSpecs(
								 propertyEntityIds, rows, rowCount, storagePredicates_, threadPool_, options);
	}

} // namespace graph::query::execution
