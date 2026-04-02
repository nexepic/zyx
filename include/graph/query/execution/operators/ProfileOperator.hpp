/**
 * @file ProfileOperator.hpp
 * @brief Physical operator for PROFILE — executes the query with PerfTrace enabled
 *        and appends timing rows after the result.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <string>
#include "../PhysicalOperator.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace graph::query::execution::operators {

	class ProfileOperator : public PhysicalOperator {
	public:
		explicit ProfileOperator(std::unique_ptr<PhysicalOperator> innerOp) :
			innerOp_(std::move(innerOp)) {}

		void open() override {
			debug::PerfTrace::setEnabled(true);
			debug::PerfTrace::reset();
			if (innerOp_) innerOp_->open();
			phase_ = Phase::RESULTS;
		}

		std::optional<RecordBatch> next() override {
			if (phase_ == Phase::RESULTS && innerOp_) {
				auto batch = innerOp_->next();
				if (batch.has_value()) {
					return batch;
				}
				// Inner plan exhausted, switch to profiling phase
				phase_ = Phase::PROFILING;
			}

			if (phase_ == Phase::PROFILING) {
				phase_ = Phase::DONE;
				auto snapshot = debug::PerfTrace::snapshotAndReset();
				debug::PerfTrace::setEnabled(false);

				RecordBatch batch;
				for (const auto &[key, entry] : snapshot) {
					Record r;
					r.setValue("phase", PropertyValue(key));
					double totalMs = static_cast<double>(entry.totalNs) / 1e6;
					r.setValue("total_time_ms", PropertyValue(totalMs));
					r.setValue("calls", PropertyValue(static_cast<int64_t>(entry.calls)));
					batch.push_back(std::move(r));
				}

				if (batch.empty())
					return std::nullopt;
				return batch;
			}

			return std::nullopt;
		}

		void close() override {
			if (innerOp_) innerOp_->close();
			debug::PerfTrace::setEnabled(false);
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			// Return inner operator's columns + profiling columns
			if (innerOp_) {
				auto cols = innerOp_->getOutputVariables();
				// Profiling rows will add: phase, total_time_ms, calls
				return cols;
			}
			return {"phase", "total_time_ms", "calls"};
		}

		[[nodiscard]] std::string toString() const override { return "Profile"; }

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (innerOp_) return {innerOp_.get()};
			return {};
		}

	private:
		std::unique_ptr<PhysicalOperator> innerOp_;
		enum class Phase { RESULTS, PROFILING, DONE };
		Phase phase_ = Phase::RESULTS;
	};

} // namespace graph::query::execution::operators
