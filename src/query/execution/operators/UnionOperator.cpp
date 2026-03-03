/**
 * @file UnionOperator.cpp
 * @author Metrix Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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

#include "graph/query/execution/operators/UnionOperator.hpp"
#include <stdexcept>
#include <string>

namespace graph::query::execution::operators {

UnionOperator::UnionOperator(std::unique_ptr<PhysicalOperator> left,
                             std::unique_ptr<PhysicalOperator> right,
                             bool all)
	: left_(std::move(left)), right_(std::move(right)), all_(all),
	  state_(State::NOT_OPENED), bufferedRightIndex_(0) {
	if (!left_) {
		throw std::runtime_error("UnionOperator: left operator cannot be null");
	}
	if (!right_) {
		throw std::runtime_error("UnionOperator: right operator cannot be null");
	}

	// Validate that both queries have the same output columns
	auto leftVars = left_->getOutputVariables();
	auto rightVars = right_->getOutputVariables();

	if (leftVars != rightVars) {
		std::string msg = "UnionOperator: left and right queries must have the same output columns.\nLeft: [";
		for (const auto& v : leftVars) msg += v + " ";
		msg += "], Right: [";
		for (const auto& v : rightVars) msg += v + " ";
		msg += "]";

		throw std::runtime_error(msg);
	}

	// Store output variables for serialization
	outputVariables_ = leftVars;
}

void UnionOperator::open() {
	// Reset state if reopening
	if (state_ == State::FINISHED) {
		state_ = State::NOT_OPENED;
		seenRecords_.clear();
		bufferedRightRecords_.clear();
		bufferedRightIndex_ = 0;
	}

	if (state_ != State::NOT_OPENED) {
		throw std::runtime_error("UnionOperator::open() called in invalid state");
	}

	left_->open();
	right_->open();

	// For UNION, we need to buffer all right query results for deduplication
	if (!all_) {
		// Consume all records from right query and buffer them
		while (auto batch = right_->next()) {
			for (auto& record : *batch) {
				bufferedRightRecords_.push_back(std::move(record));
			}
		}
	}

	state_ = State::CONSUMING_LEFT;
}

std::optional<RecordBatch> UnionOperator::next() {
	switch (state_) {
		case State::NOT_OPENED:
			throw std::runtime_error("UnionOperator::next() called before open()");

		case State::CONSUMING_LEFT: {
			auto batch = left_->next();
			if (batch) {
				if (!all_) {
					// For UNION: mark all records from left as seen
					for (const auto& record : *batch) {
						seenRecords_.insert(serializeRecord(record));
					}
				}
				return batch;
			}

			// Left query exhausted, move to right
			state_ = State::CONSUMING_RIGHT;
			bufferedRightIndex_ = 0;

			// Fall through to consuming right
		}

		case State::CONSUMING_RIGHT: {
			if (all_) {
				// UNION ALL: return records from right query directly
				auto batch = right_->next();
				if (batch) {
					return batch;
				}
				state_ = State::FINISHED;
				return std::nullopt;
			} else {
				// UNION: return buffered records from right, filtering duplicates
				RecordBatch outputBatch;
				outputBatch.reserve(bufferedRightRecords_.size());

				while (bufferedRightIndex_ < bufferedRightRecords_.size()) {
					const auto& record = bufferedRightRecords_[bufferedRightIndex_++];
					std::string serialized = serializeRecord(record);

					// Only include records not seen in left query
					if (seenRecords_.find(serialized) == seenRecords_.end()) {
						outputBatch.push_back(record);
						seenRecords_.insert(serialized);
					}

					// Return batch if we've collected enough records
					if (outputBatch.size() >= 100) { // Arbitrary batch size
						return outputBatch;
					}
				}

				if (!outputBatch.empty()) {
					return outputBatch;
				}

				state_ = State::FINISHED;
				return std::nullopt;
			}
		}

		case State::FINISHED:
			return std::nullopt;

		default:
			throw std::runtime_error("UnionOperator: invalid state");
	}
}

void UnionOperator::close() {
	if (left_) {
		left_->close();
	}
	if (right_) {
		right_->close();
	}
	state_ = State::FINISHED;
}

std::vector<std::string> UnionOperator::getOutputVariables() const {
	return left_ ? left_->getOutputVariables() : std::vector<std::string>{};
}

std::string UnionOperator::toString() const {
	return "UnionOperator(" + std::string(all_ ? "ALL" : "UNIQUE") + ")";
}

std::vector<const PhysicalOperator *> UnionOperator::getChildren() const {
	std::vector<const PhysicalOperator *> children;
	if (left_) children.push_back(left_.get());
	if (right_) children.push_back(right_.get());
	return children;
}

std::string UnionOperator::serializeRecord(const Record& record) const {
	// Serialize record to string for hash comparison
	// Format: "value1|value2|value3|..." where values are converted to strings

	std::string result;
	bool first = true;

	for (const auto& var : outputVariables_) {
		if (!first) {
			result += "|";
		}
		first = false;

		auto value = record.getValue(var);
		if (value.has_value()) {
			result += value->toString();
		} else {
			result += "NULL";
		}
	}

	return result;
}

} // namespace graph::query::execution::operators
