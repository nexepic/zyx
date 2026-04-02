/**
 * @file ResetStatsOperator.hpp
 * @brief Physical operator for CALL dbms.resetStats() — resets runtime counters.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <string>
#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class ResetStatsOperator : public PhysicalOperator {
	public:
		ResetStatsOperator(std::shared_ptr<storage::DataManager> dm,
						   std::shared_ptr<indexes::IndexManager> im) :
			dm_(std::move(dm)), im_(std::move(im)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;
			executed_ = true;

			dm_->getPagePool().resetStats();
			im_->resetStats();

			RecordBatch batch;
			Record r;
			r.setValue("result", PropertyValue(std::string("Statistics reset")));
			batch.push_back(std::move(r));
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"result"};
		}

		[[nodiscard]] std::string toString() const override { return "ResetStats()"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
