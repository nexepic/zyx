/**
 * @file RecordInjectorOperator.hpp
 * @brief Utility operator that emits a single injected record.
 *
 * Used by ForeachOperator and CallSubqueryOperator to seed inner pipelines
 * with context from the outer query (e.g., iterator variables, imported vars).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/execution/PhysicalOperator.hpp"

namespace graph::query::execution::operators {

class RecordInjectorOperator : public PhysicalOperator {
public:
	RecordInjectorOperator() = default;

	/// Set the record to inject before each open()/next() cycle.
	void setRecord(const Record &record) { record_ = record; }

	void open() override { emitted_ = false; }

	std::optional<RecordBatch> next() override {
		if (emitted_) return std::nullopt;
		emitted_ = true;
		RecordBatch batch;
		batch.push_back(record_);
		return batch;
	}

	void close() override { emitted_ = true; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
	[[nodiscard]] std::string toString() const override { return "RecordInjector"; }
	[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {}; }

private:
	Record record_;
	bool emitted_ = false;
};

} // namespace graph::query::execution::operators
