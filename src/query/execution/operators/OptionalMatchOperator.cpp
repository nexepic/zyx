/**
 * @file OptionalMatchOperator.cpp
 * @author ZYX Graph Database Team
 * @date 2025
 *
 * @copyright Copyright (c) 2025 ZYX Graph Database
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/query/execution/operators/OptionalMatchOperator.hpp"
#include "graph/query/QueryContext.hpp"
#include <stdexcept>

namespace graph::query::execution::operators {

OptionalMatchOperator::OptionalMatchOperator(
	std::unique_ptr<PhysicalOperator> input,
	std::unique_ptr<PhysicalOperator> optionalPattern,
	std::vector<std::string> requiredVariables)
	: input_(std::move(input))
	, optionalPattern_(std::move(optionalPattern))
	, requiredVariables_(std::move(requiredVariables))
	, currentInputIndex_(0)
	, inputExhausted_(false) {
	if (!input_) {
		throw std::invalid_argument("Input operator cannot be null");
	}
	if (!optionalPattern_) {
		throw std::invalid_argument("Optional pattern operator cannot be null");
	}
}

void OptionalMatchOperator::open() {
	input_->open();
	optionalPattern_->open();
	currentInputIndex_ = 0;
	inputExhausted_ = false;
	currentInputBatch_.clear();
}

std::optional<RecordBatch> OptionalMatchOperator::next() {
	RecordBatch resultBatch;

	// Process until we exhaust input
	while (!inputExhausted_) {
		if (queryContext_) queryContext_->checkGuard();

		// Need to fetch new input batch?
		if (currentInputIndex_ >= currentInputBatch_.size()) {
			auto batchOpt = input_->next();
			if (!batchOpt || batchOpt->empty()) {
				inputExhausted_ = true;
				break;
			}
			currentInputBatch_ = std::move(*batchOpt);
			currentInputIndex_ = 0;
		}

		// Process current input record
		const Record& inputRecord = currentInputBatch_[currentInputIndex_++];

		// Try to match the optional pattern
		auto matches = tryMatchPattern(inputRecord);

		if (matches.empty()) {
			// No match: create NULL-extended record
			resultBatch.push_back(createNullExtendedRecord(inputRecord));
		} else {
			// Matches found: extend input record with each match
			for (const auto& match : matches) {
				Record combinedRecord = inputRecord;
				// Merge all data from the match
				combinedRecord.merge(match);
				resultBatch.push_back(std::move(combinedRecord));
			}
		}

		// To avoid producing overly large batches, limit batch size
		if (resultBatch.size() >= 1000) {
			break;
		}
	}

	if (resultBatch.empty()) {
		return std::nullopt;
	}

	return resultBatch;
}

void OptionalMatchOperator::close() {
	if (input_) {
		input_->close();
	}
	if (optionalPattern_) {
		optionalPattern_->close();
	}
	currentInputBatch_.clear();
	currentInputIndex_ = 0;
	inputExhausted_ = false;
}

std::vector<std::string> OptionalMatchOperator::getOutputVariables() const {
	auto inputSchema = input_->getOutputVariables();
	auto patternSchema = optionalPattern_->getOutputVariables();

	// Combine schemas (input + pattern)
	std::vector<std::string> combinedSchema = inputSchema;
	combinedSchema.insert(combinedSchema.end(), patternSchema.begin(), patternSchema.end());

	return combinedSchema;
}

std::string OptionalMatchOperator::toString() const {
	std::string variablesStr;
	for (size_t i = 0; i < requiredVariables_.size(); ++i) {
		if (i > 0) variablesStr += ", ";
		variablesStr += requiredVariables_[i];
	}

	return "OptionalMatchOperator(required_vars=[" + variablesStr + "])";
}

Record OptionalMatchOperator::createNullExtendedRecord(const Record& inputRecord) {
	Record result = inputRecord;

	// Add NULL values for all required variables
	// Note: We use setValue which creates NULL PropertyValue
	// The record won't have these variables, so we set them as NULL
	for (const auto& varName : requiredVariables_) {
		// Check if variable exists as node
		auto nodeOpt = result.getNode(varName);
		if (!nodeOpt.has_value()) {
			// Check if variable exists as edge
			auto edgeOpt = result.getEdge(varName);
			if (!edgeOpt.has_value()) {
				// Variable doesn't exist, set as NULL value
				// This marks the variable as present but NULL
				result.setValue(varName, PropertyValue());
			}
		}
	}

	return result;
}

std::vector<Record> OptionalMatchOperator::tryMatchPattern(const Record& /*inputRecord*/) {
	std::vector<Record> matches;

	// For OPTIONAL MATCH, we need to execute the pattern matcher
	// The pattern operator should work independently and return matches
	//
	// Note: This is a simplified implementation. A full implementation would:
	// 1. Pass the input record context to the pattern matcher
	// 2. Bind variables from the input record
	// 3. Execute the pattern with those bindings
	// 4. Return all matching records

	// For the current implementation, we'll get records from the pattern operator
	auto batchOpt = optionalPattern_->next();
	if (batchOpt) {
		for (const auto& record : *batchOpt) {
			matches.push_back(record);
		}
	}

	// If the pattern operator returns no records, we return empty vector
	// This signals that no match was found, and NULL extension should occur

	return matches;
}

} // namespace graph::query::execution::operators
