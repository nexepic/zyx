/**
 * @file FilterOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"

namespace graph::query::execution::operators {

	class FilterOperator : public PhysicalOperator {
	public:
		using Predicate = std::function<bool(const Record &)>;

		/**
		 * @brief Constructs a FilterOperator.
		 *
		 * @param child The upstream operator.
		 * @param predicate The generic lambda function for filtering.
		 * @param predicateStr A string representation of the logic (for debugging/visualization).
		 */
		FilterOperator(std::unique_ptr<PhysicalOperator> child, Predicate predicate, std::string predicateStr) :
			child_(std::move(child)), predicate_(std::move(predicate)), predicateStr_(std::move(predicateStr)) {}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			// Standard Filter Logic:
			// Keep pulling batches from child until we find matching records or child is exhausted.
			while (true) {
				auto batchOpt = child_->next();
				if (!batchOpt)
					return std::nullopt; // End of stream

				RecordBatch &inputBatch = *batchOpt;
				RecordBatch outputBatch;

				// Optimization: Reserve memory to avoid reallocations
				outputBatch.reserve(inputBatch.size());

				for (auto &record: inputBatch) {
					if (predicate_(record)) {
						outputBatch.push_back(std::move(record));
					}
				}

				// If we found valid records, return them.
				if (!outputBatch.empty()) {
					return outputBatch;
				}

				// If outputBatch is empty, it means the entire input batch was filtered out.
				// We loop again immediately to fetch the next batch.
			}
		}

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
		}

		// --- Visualization ---
		[[nodiscard]] std::string toString() const override { return "Filter(" + predicateStr_ + ")"; }

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_)
				return {child_.get()};
			return {};
		}

	private:
		std::unique_ptr<PhysicalOperator> child_;
		Predicate predicate_;
		std::string predicateStr_;
	};

} // namespace graph::query::execution::operators
