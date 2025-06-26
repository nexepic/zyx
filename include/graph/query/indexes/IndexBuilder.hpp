/**
 * @file IndexBuilder.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace graph::storage {
	class DataManager;
	class FileStorage;
} // namespace graph::storage

namespace graph::query::indexes {

	class IndexManager;
	class LabelIndex;
	class PropertyIndex;
	class RelationshipIndex;
	class FullTextIndex;

	// Enum for index building status
	enum class IndexBuildStatus { NOT_STARTED, IN_PROGRESS, COMPLETED, FAILED };

	// Class to handle background index building with progress tracking
	class IndexBuilder {
	public:
		explicit IndexBuilder(std::shared_ptr<IndexManager> indexManager,
							  std::shared_ptr<storage::FileStorage> storage);
		~IndexBuilder();

		// Start building all indexes in background
		bool startBuildAllIndexes();

		// Start building label index in background
		bool startBuildLabelIndex();

		// Start building property index in background
		bool startBuildPropertyIndex(const std::string &key);

		// Check if an index build is in progress
		bool isBuilding() const;

		// Get the current status of index building
		IndexBuildStatus getStatus() const;

		// Get the progress percentage (0-100)
		int getProgress() const;

		// Wait for completion of the current index building task
		bool waitForCompletion(std::chrono::seconds timeout = std::chrono::seconds(60));

		// Cancel the current index building task
		void cancel();

		// Worker functions
		bool buildAllIndexesWorker();
		bool buildLabelIndexWorker();
		bool buildPropertyIndexWorker(const std::string &key);

	private:
		// Batch size for processing nodes/edges
		static constexpr size_t BATCH_SIZE = 10000;

		std::shared_ptr<IndexManager> indexManager_;
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<storage::DataManager> dataManager_;

		// Task management
		std::future<bool> buildTask_;
		std::atomic<bool> cancelRequested_{false};
		std::atomic<IndexBuildStatus> status_{IndexBuildStatus::NOT_STARTED};
		std::atomic<int> progress_{0};

		// Process a batch of nodes for indexing
		void processNodeBatch(std::vector<int64_t> &nodeIds, std::shared_ptr<LabelIndex> labelIndex,
							  std::shared_ptr<PropertyIndex> propertyIndex,
							  std::shared_ptr<FullTextIndex> fullTextIndex, const std::string &propertyKey = "");

		// Process a batch of edges for indexing
		void processEdgeBatch(std::vector<int64_t> &edgeIds, std::shared_ptr<RelationshipIndex> relationshipIndex);

		// Get node ID ranges from segments for efficient batch processing
		std::vector<std::pair<int64_t, int64_t>> getNodeIdRanges() const;

		// Get edge ID ranges from segments for efficient batch processing
		std::vector<std::pair<int64_t, int64_t>> getEdgeIdRanges() const;

		// Update progress tracking
		void updateProgress(int newProgress);
	};

} // namespace graph::query::indexes
