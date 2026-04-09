/**
 * @file CallSubqueryOperator.hpp
 * @brief Physical operator for CALL { ... } inline subqueries.
 *
 * For each input row, injects imported variables into the subquery context,
 * executes the subquery, and merges results with the input row.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/operators/RecordInjectorOperator.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace graph::query::execution::operators {

class CallSubqueryOperator : public PhysicalOperator {
public:
	CallSubqueryOperator(std::unique_ptr<PhysicalOperator> input,
	                     std::unique_ptr<PhysicalOperator> subquery,
	                     std::vector<std::string> importedVars,
	                     std::vector<std::string> returnedVars,
	                     RecordInjectorOperator *injector,
	                     bool inTransactions = false,
	                     int64_t batchSize = 0)
		: input_(std::move(input)), subquery_(std::move(subquery)),
		  importedVars_(std::move(importedVars)), returnedVars_(std::move(returnedVars)),
		  injector_(injector),
		  inTransactions_(inTransactions), batchSize_(batchSize) {}

	void open() override {
		if (input_) input_->open();
		bufferedOutput_.clear();
		bufferIndex_ = 0;
		seeded_ = false;
	}

	std::optional<RecordBatch> next() override {
		RecordBatch result;
		result.reserve(DEFAULT_BATCH_SIZE);

		while (result.size() < DEFAULT_BATCH_SIZE) {
			// Drain buffered output first
			while (bufferIndex_ < bufferedOutput_.size() && result.size() < DEFAULT_BATCH_SIZE) {
				result.push_back(std::move(bufferedOutput_[bufferIndex_++]));
			}
			if (result.size() >= DEFAULT_BATCH_SIZE) break;

			// Get next input batch
			RecordBatch inputBatch;
			if (input_) {
				auto batch = input_->next();
				if (!batch) break;
				inputBatch = std::move(*batch);
			} else {
				// Standalone CALL: generate a single empty seed record
				if (seeded_) break;
				seeded_ = true;
				inputBatch.emplace_back();
			}

			// Reset buffer for new batch
			bufferedOutput_.clear();
			bufferIndex_ = 0;

			// Process each input row
			for (auto &inputRecord : inputBatch) {
				executeSubqueryForRecord(inputRecord);
			}
		}

		if (result.empty()) return std::nullopt;
		return result;
	}

	void close() override {
		if (input_) input_->close();
		bufferedOutput_.clear();
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		auto vars = input_ ? input_->getOutputVariables() : std::vector<std::string>{};
		vars.insert(vars.end(), returnedVars_.begin(), returnedVars_.end());
		return vars;
	}

	[[nodiscard]] std::string toString() const override {
		std::string s = "CallSubquery";
		if (inTransactions_) {
			s += "(IN TRANSACTIONS";
			if (batchSize_ > 0) s += " OF " + std::to_string(batchSize_) + " ROWS";
			s += ")";
		}
		return s;
	}

	[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
		std::vector<const PhysicalOperator *> children;
		if (input_) children.push_back(input_.get());
		if (subquery_) children.push_back(subquery_.get());
		return children;
	}

private:
	std::unique_ptr<PhysicalOperator> input_;
	std::unique_ptr<PhysicalOperator> subquery_;
	std::vector<std::string> importedVars_;
	std::vector<std::string> returnedVars_;
	RecordInjectorOperator *injector_;
	bool inTransactions_;
	int64_t batchSize_;
	bool seeded_ = false;

	// Buffer for merged output records
	std::vector<Record> bufferedOutput_;
	size_t bufferIndex_ = 0;

	void executeSubqueryForRecord(const Record &inputRecord) {
		if (!subquery_) return;

		// Inject imported variables into the subquery via RecordInjector
		if (injector_) {
			Record injectedRecord;
			for (const auto &var : importedVars_) {
				if (auto val = inputRecord.getValue(var)) {
					injectedRecord.setValue(var, *val);
				}
			}
			injector_->setRecord(injectedRecord);
		}

		subquery_->open();

		// Collect all subquery results
		bool hasResults = false;
		while (auto subBatch = subquery_->next()) {
			for (auto &subRecord : *subBatch) {
				hasResults = true;
				// Merge input record with subquery result
				Record merged = inputRecord;
				merged.merge(subRecord);
				bufferedOutput_.push_back(std::move(merged));
			}
		}

		subquery_->close();

		// If subquery produced no results (write-only subquery),
		// pass through the input record unchanged
		if (!hasResults && returnedVars_.empty()) {
			bufferedOutput_.push_back(inputRecord);
		}
	}
};

} // namespace graph::query::execution::operators
