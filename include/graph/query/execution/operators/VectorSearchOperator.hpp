/**
 * @file VectorSearchOperator.hpp
 * @author Nexepic
 * @date 2026/1/23
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

#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

namespace graph::query::execution::operators {

	class VectorSearchOperator : public PhysicalOperator {
	public:
		VectorSearchOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
							 std::string indexName, int64_t topK, std::vector<float> queryVector) :
			dm_(std::move(dm)), im_(std::move(im)), indexName_(std::move(indexName)), topK_(topK),
			queryVector_(std::move(queryVector)) {}

		void open() override {}

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;
			executed_ = true;

			auto vim = im_->getVectorIndexManager();
			if (!vim->hasIndex(indexName_)) {
				throw std::runtime_error("Vector index not found: " + indexName_);
			}

			auto index = vim->getIndex(indexName_);
			// Perform search via DiskANN
			auto results = index->search(queryVector_, topK_);

			RecordBatch batch;
			batch.reserve(results.size());

			for (const auto &[nodeId, score] : results) {
				auto node = dm_->getNode(nodeId);

				if (node.getId() == 0 || !node.isActive()) {
					continue;
				}

				// Hydrate properties!
				// Get all properties (inline + external)
				auto props = dm_->getNodeProperties(nodeId);
				node.setProperties(std::move(props));

				Record record;
				record.setNode("node", node);
				record.setValue("score", PropertyValue(score));
				batch.push_back(std::move(record));
			}

			return batch;
		}

		void close() override {}

		std::vector<std::string> getOutputVariables() const override { return {"node", "score"}; }

		std::string toString() const override {
			return "VectorSearch(" + indexName_ + ", k=" + std::to_string(topK_) + ")";
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		std::string indexName_;
		int64_t topK_;
		std::vector<float> queryVector_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
