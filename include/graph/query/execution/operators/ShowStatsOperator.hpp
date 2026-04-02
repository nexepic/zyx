/**
 * @file ShowStatsOperator.hpp
 * @brief Physical operator for CALL dbms.showStats() — returns runtime statistics.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstdint>
#include <string>
#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class ShowStatsOperator : public PhysicalOperator {
	public:
		struct CacheStats {
			uint64_t hits = 0;
			uint64_t misses = 0;
		};

		ShowStatsOperator(std::shared_ptr<storage::DataManager> dm,
						  std::shared_ptr<indexes::IndexManager> im,
						  CacheStats planCacheStats) :
			dm_(std::move(dm)), im_(std::move(im)), planCacheStats_(planCacheStats) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;
			executed_ = true;

			RecordBatch batch;

			// Page buffer pool stats
			auto &pool = dm_->getPagePool();
			addRow(batch, "cache", "page_pool_hits", static_cast<int64_t>(pool.hits()));
			addRow(batch, "cache", "page_pool_misses", static_cast<int64_t>(pool.misses()));
			auto totalCache = pool.hits() + pool.misses();
			double cacheHitRate = totalCache > 0 ? static_cast<double>(pool.hits()) / static_cast<double>(totalCache) : 0.0;
			addRow(batch, "cache", "page_pool_hit_rate", cacheHitRate);

			// Index stats
			addRow(batch, "index", "lookups", static_cast<int64_t>(im_->lookups()));
			addRow(batch, "index", "hits", static_cast<int64_t>(im_->indexHits()));
			auto totalIndex = im_->lookups();
			double indexHitRate = totalIndex > 0 ? static_cast<double>(im_->indexHits()) / static_cast<double>(totalIndex) : 0.0;
			addRow(batch, "index", "hit_rate", indexHitRate);

			// Plan cache stats
			addRow(batch, "plan_cache", "hits", static_cast<int64_t>(planCacheStats_.hits));
			addRow(batch, "plan_cache", "misses", static_cast<int64_t>(planCacheStats_.misses));
			auto totalPlan = planCacheStats_.hits + planCacheStats_.misses;
			double planHitRate = totalPlan > 0 ? static_cast<double>(planCacheStats_.hits) / static_cast<double>(totalPlan) : 0.0;
			addRow(batch, "plan_cache", "hit_rate", planHitRate);

			if (batch.empty())
				return std::nullopt;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"category", "metric", "value"};
		}

		[[nodiscard]] std::string toString() const override { return "ShowStats()"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		CacheStats planCacheStats_;
		bool executed_ = false;

		static void addRow(RecordBatch &batch, const std::string &category,
						   const std::string &metric, int64_t value) {
			Record r;
			r.setValue("category", PropertyValue(category));
			r.setValue("metric", PropertyValue(metric));
			r.setValue("value", PropertyValue(value));
			batch.push_back(std::move(r));
		}

		static void addRow(RecordBatch &batch, const std::string &category,
						   const std::string &metric, double value) {
			Record r;
			r.setValue("category", PropertyValue(category));
			r.setValue("metric", PropertyValue(metric));
			r.setValue("value", PropertyValue(value));
			batch.push_back(std::move(r));
		}
	};

} // namespace graph::query::execution::operators
