/**
 * @file IndexManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include "EntityTypeIndexManager.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph::query::indexes {

	class IndexBuilder;

	// Define constants for index types and state keys
	namespace IndexTypes {
		constexpr uint32_t NODE_LABEL_TYPE = 1;
		constexpr uint32_t NODE_PROPERTY_TYPE = 2;
		constexpr uint32_t EDGE_LABEL_TYPE = 3;
		constexpr uint32_t EDGE_PROPERTY_TYPE = 4;
	}

	namespace StateKeys {
		// Keys for index data (B-Tree roots)
		constexpr char NODE_LABEL_ROOT[] = "node.index.label_root";
		constexpr char NODE_PROPERTY_PREFIX[] = "node.index.property";
		constexpr char EDGE_LABEL_ROOT[] = "edge.index.label_root";
		constexpr char EDGE_PROPERTY_PREFIX[] = "edge.index.property";
	}

	class IndexManager : public std::enable_shared_from_this<IndexManager> {
	public:
		explicit IndexManager(std::shared_ptr<storage::FileStorage> storage);
		~IndexManager();

		void initialize();

		// Index creation methods now take an entity type
		bool buildIndexes(const std::string& entityType);
		bool buildPropertyIndex(const std::string& entityType, const std::string& key);

		// Drop index method
		bool dropIndex(const std::string& entityType, const std::string& indexType, const std::string& key = "");

		// List indexes for a specific entity type
		std::vector<std::pair<std::string, std::string>> listIndexes(const std::string& entityType) const;

		void persistState() const;

		// --- Entity Event Handlers ---
		void onNodeAdded(const Node& node) const;
		void onNodeUpdated(const Node& oldNode, const Node& newNode) const;
		void onNodeDeleted(const Node& node) const;
		void onEdgeAdded(const Edge& edge) const;
		void onEdgeUpdated(const Edge& oldEdge, const Edge& newEdge) const;
		void onEdgeDeleted(const Edge& edge) const;

		// --- Accessors ---
		std::shared_ptr<EntityTypeIndexManager> getNodeIndexManager() const { return nodeIndexManager_; }
		std::shared_ptr<EntityTypeIndexManager> getEdgeIndexManager() const { return edgeIndexManager_; }
		IndexBuilder* getIndexBuilder() const { return indexBuilder_.get(); }

		// Query methods now need to know which index to use
		std::vector<int64_t> findNodeIdsByLabel(const std::string &label) const;
		std::vector<int64_t> findNodeIdsByProperty(const std::string &key, const std::string &value) const;

        std::vector<int64_t> findEdgeIdsByLabel(const std::string &label) const;
        std::vector<int64_t> findEdgeIdsByProperty(const std::string &key, const std::string &value) const;

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<storage::DataManager> dataManager_;

		std::shared_ptr<EntityTypeIndexManager> nodeIndexManager_;
		std::shared_ptr<EntityTypeIndexManager> edgeIndexManager_;

		std::unique_ptr<IndexBuilder> indexBuilder_;
		mutable std::recursive_mutex mutex_;

		bool executeBuildTask(const std::function<bool()>& buildFunc) const;
	};

} // namespace graph::query::indexes