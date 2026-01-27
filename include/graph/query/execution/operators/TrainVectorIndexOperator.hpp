/**
 * @file TrainVectorIndexOperator.hpp
 * @author Nexepic
 * @date 2026/1/26
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#pragma once

#include <chrono>
#include "graph/log/Log.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

namespace graph::query::execution::operators {

	class TrainVectorIndexOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Operator to manually trigger vector index training.
		 * Usage: CALL db.index.vector.train('index_name')
		 */
		TrainVectorIndexOperator(std::shared_ptr<indexes::IndexManager> im, std::string indexName) :
			im_(std::move(im)), indexName_(std::move(indexName)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			auto vim = im_->getVectorIndexManager();
			if (!vim->hasIndex(indexName_)) {
				throw std::runtime_error("Vector index not found: " + indexName_);
			}

			auto index = vim->getIndex(indexName_);

			// 1. Check if already trained (Optional: allow re-training?)
			// We allow re-training to optimize codebooks as data grows.

			// 2. Sampling
			// Sample size: 5000 is a good balance for accuracy vs speed.
			// If total data < 5000, it will just take all available.
			size_t sampleSize = 5000;

			log::Log::info("Sampling {} vectors for training index '{}'...", sampleSize, indexName_);
			auto samples = index->sampleVectors(sampleSize);

			if (samples.empty()) {
				log::Log::warn("Index '{}' has no data. Training skipped.", indexName_);
				// Return success message but indicate no action
				return createResultBatch("Skipped (Empty Data)");
			}

			// 3. Train
			log::Log::info("Starting training on {} samples...", samples.size());
			auto start = std::chrono::high_resolution_clock::now();

			index->train(samples);

			auto end = std::chrono::high_resolution_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			log::Log::info("Training completed in {} ms.", duration);

			executed_ = true;
			return createResultBatch("Success");
		}

		void close() override {}

		std::vector<std::string> getOutputVariables() const override {
			// Returns a status message
			return {"status"};
		}

		std::string toString() const override { return "TrainVectorIndex(" + indexName_ + ")"; }

	private:
		std::shared_ptr<indexes::IndexManager> im_;
		std::string indexName_;
		bool executed_ = false;

		// Helper to create a single-row result
		static RecordBatch createResultBatch(const std::string &status) {
			RecordBatch batch;
			Record r;
			r.setValue("status", PropertyValue(status));
			batch.push_back(std::move(r));
			return batch;
		}
	};

} // namespace graph::query::execution::operators
