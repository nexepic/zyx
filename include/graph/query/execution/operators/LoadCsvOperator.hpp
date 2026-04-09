/**
 * @file LoadCsvOperator.hpp
 * @brief Physical operator for LOAD CSV statement.
 *
 * Streaming CSV reader that emits one row per CSV line,
 * binding the row variable to either a list (no headers) or map (with headers).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/CsvReader.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace graph::query::execution::operators {

class LoadCsvOperator : public PhysicalOperator {
public:
	LoadCsvOperator(std::unique_ptr<PhysicalOperator> child,
	                std::shared_ptr<graph::query::expressions::Expression> urlExpr,
	                std::string rowVariable,
	                bool withHeaders,
	                std::string fieldTerminator)
		: child_(std::move(child)), urlExpr_(std::move(urlExpr)),
		  rowVariable_(std::move(rowVariable)), withHeaders_(withHeaders),
		  fieldTerminator_(std::move(fieldTerminator)) {}

	void open() override {
		if (child_) child_->open();

		// Evaluate URL expression to get file path
		Record emptyRecord;
		auto urlVal = expressions::ExpressionEvaluationHelper::evaluate(
			urlExpr_.get(), emptyRecord, nullptr);

		std::string filePath;
		if (urlVal.getType() == graph::PropertyType::STRING) {
			filePath = urlVal.toString();
		} else {
			throw std::runtime_error("LOAD CSV: URL expression must evaluate to a string");
		}

		// Strip file:/// prefix
		const std::string filePrefix = "file:///";
		if (filePath.substr(0, filePrefix.size()) == filePrefix) {
			filePath = "/" + filePath.substr(filePrefix.size());
		}

		reader_ = std::make_unique<CsvReader>(filePath, fieldTerminator_);

		// Read headers if needed
		if (withHeaders_ && reader_->hasNext()) {
			headers_ = reader_->nextRow();
		}
	}

	std::optional<RecordBatch> next() override {
		RecordBatch batch;
		batch.reserve(DEFAULT_BATCH_SIZE);

		while (batch.size() < DEFAULT_BATCH_SIZE && reader_ && reader_->hasNext()) {
			auto fields = reader_->nextRow();

			Record record;
			// Merge with child record if we have a child (pipeline input)
			// For standalone LOAD CSV, no child needed

			if (withHeaders_) {
				// Create a map value: {header1: val1, header2: val2, ...}
				std::unordered_map<std::string, PropertyValue> map;
				for (size_t i = 0; i < headers_.size() && i < fields.size(); ++i) {
					map[headers_[i]] = PropertyValue(fields[i]);
				}
				record.setValue(rowVariable_, PropertyValue(std::move(map)));
			} else {
				// Create a list value: [val1, val2, ...]
				std::vector<PropertyValue> list;
				list.reserve(fields.size());
				for (auto &f : fields) {
					list.emplace_back(std::move(f));
				}
				record.setValue(rowVariable_, PropertyValue(std::move(list)));
			}

			batch.push_back(std::move(record));
		}

		if (batch.empty()) return std::nullopt;
		return batch;
	}

	void close() override {
		reader_.reset();
		if (child_) child_->close();
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
		vars.push_back(rowVariable_);
		return vars;
	}

	[[nodiscard]] std::string toString() const override {
		return "LoadCsv(" + rowVariable_ + (withHeaders_ ? ", WITH HEADERS" : "") + ")";
	}

	[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
		if (child_) return {child_.get()};
		return {};
	}

private:
	std::unique_ptr<PhysicalOperator> child_;
	std::shared_ptr<graph::query::expressions::Expression> urlExpr_;
	std::string rowVariable_;
	bool withHeaders_;
	std::string fieldTerminator_;
	std::unique_ptr<CsvReader> reader_;
	std::vector<std::string> headers_;
};

} // namespace graph::query::execution::operators
