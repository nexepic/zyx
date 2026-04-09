/**
 * @file IndexManager.hpp
 * @author Nexepic
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "IEntityObserver.hpp"
#include "graph/storage/IStorageEventListener.hpp"

namespace graph::storage {
	class DataManager;
	class FileStorage;
} // namespace graph::storage
namespace graph::query::indexes {
	class VectorIndexManager;
	class EntityTypeIndexManager;

	class IndexBuilder;

	// Define constants for index types and state keys
	namespace IndexTypes {
		constexpr uint32_t NODE_LABEL_TYPE = 1;
		constexpr uint32_t NODE_PROPERTY_TYPE = 2;
		constexpr uint32_t EDGE_TYPE_INDEX = 3;
		constexpr uint32_t EDGE_PROPERTY_TYPE = 4;
		constexpr uint32_t SYSTEM_LABEL_DICTIONARY = 5;
		constexpr uint32_t VECTOR_MAPPING_INDEX = 6;
		constexpr uint32_t COMPOSITE_NODE_PROPERTY_TYPE = 7;
	} // namespace IndexTypes

	namespace StateKeys {
		// Keys for index data (B-Tree roots)
		constexpr char NODE_LABEL_ROOT[] = "node.index.label_root";
		constexpr char NODE_PROPERTY_PREFIX[] = "node.index.property";
		constexpr char EDGE_TYPE_ROOT[] = "edge.index.type_root";
		constexpr char EDGE_PROPERTY_PREFIX[] = "edge.index.property";
	} // namespace StateKeys

	class IndexManager : public IEntityObserver,
						 public storage::IStorageEventListener,
						 public std::enable_shared_from_this<IndexManager> {
	public:
		explicit IndexManager(std::shared_ptr<storage::FileStorage> storage);
		~IndexManager() override;

		void initialize();

		bool hasLabelIndex(const std::string &entityType) const;
		bool hasPropertyIndex(const std::string &entityType, const std::string &key) const;

		/**
		 * @brief Creates a named index and persists metadata.
		 */
		bool createIndex(const std::string &indexName, const std::string &entityType, const std::string &label,
						 const std::string &property) const;

		/**
		 * @brief Drops an index by name or definition.
		 */
		bool dropIndexByName(const std::string &indexName) const;

		/**
		 * @brief Drops an index by looking up its definition (Label + Property).
		 *        Automatically detects if it is a Node or Edge index based on metadata.
		 * @return true if found and dropped.
		 */
		bool dropIndexByDefinition(const std::string &label, const std::string &property) const;

		/**
		 * @brief Returns list of {Name, EntityType, Label, Property}
		 */
		std::vector<std::tuple<std::string, std::string, std::string, std::string>> listIndexesDetailed() const;

		/**
		 * @brief Creates a vector index and persists metadata.
		 */
		bool createVectorIndex(const std::string &indexName, const std::string &label, const std::string &property,
							   int dimension, const std::string &metric) const;

		/**
		 * @brief Gets a vector index by Label and Property (for optimizer lookup)
		 */
		std::string getVectorIndexName(const std::string &label, const std::string &property) const;

		void onStorageFlush() override;

		void persistState() const;

		// --- Entity Event Handlers (Implementing IEntityObserver) ---
		void onNodeAdded(const Node &node) override;
		void onNodesAdded(const std::vector<Node> &nodes) override;
		void onNodeUpdated(const Node &oldNode, const Node &newNode) override;
		void onNodeDeleted(const Node &node) override;
		void onEdgeAdded(const Edge &edge) override;
		void onEdgesAdded(const std::vector<Edge> &edges) override;
		void onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) override;
		void onEdgeDeleted(const Edge &edge) override;

		// --- Accessors ---
		std::shared_ptr<EntityTypeIndexManager> getNodeIndexManager() const { return nodeIndexManager_; }
		std::shared_ptr<EntityTypeIndexManager> getEdgeIndexManager() const { return edgeIndexManager_; }

		std::shared_ptr<VectorIndexManager> getVectorIndexManager() const { return vectorIndexManager_; }

		IndexBuilder *getIndexBuilder() const { return indexBuilder_.get(); }

		// Query methods now need to know which index to use
		std::vector<int64_t> findNodeIdsByLabel(const std::string &label) const;
		std::vector<int64_t> findNodeIdsByProperty(const std::string &key, const PropertyValue &value) const;
		std::vector<int64_t> findNodeIdsByPropertyRange(const std::string &key,
		                                                 const PropertyValue &minValue,
		                                                 const PropertyValue &maxValue) const;

		// Composite index API
		bool createCompositeIndex(const std::string &indexName, const std::string &entityType,
		                          const std::string &label, const std::vector<std::string> &properties) const;
		bool hasCompositeIndex(const std::string &entityType,
		                       const std::vector<std::string> &keys) const;
		std::vector<int64_t> findNodeIdsByCompositeIndex(
		    const std::vector<std::string> &keys,
		    const std::vector<PropertyValue> &values) const;

		std::vector<int64_t> findEdgeIdsByType(const std::string &type) const;
		std::vector<int64_t> findEdgeIdsByProperty(const std::string &key, const PropertyValue &value) const;

		[[nodiscard]] uint64_t lookups() const { return lookups_.load(std::memory_order_relaxed); }
		[[nodiscard]] uint64_t indexHits() const { return indexHits_.load(std::memory_order_relaxed); }
		void resetStats() {
			lookups_.store(0, std::memory_order_relaxed);
			indexHits_.store(0, std::memory_order_relaxed);
		}

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<storage::DataManager> dataManager_;

		std::shared_ptr<EntityTypeIndexManager> nodeIndexManager_;
		std::shared_ptr<EntityTypeIndexManager> edgeIndexManager_;

		std::shared_ptr<VectorIndexManager> vectorIndexManager_;

		std::unique_ptr<IndexBuilder> indexBuilder_;
		mutable std::recursive_mutex mutex_;

		bool executeBuildTask(const std::function<bool()> &buildFunc) const;

		// Composite index maintenance helpers
		void updateCompositeIndexForNode(const Node &node);
		void removeCompositeIndexForNode(const Node &node);

		mutable std::atomic<uint64_t> lookups_{0};
		mutable std::atomic<uint64_t> indexHits_{0};
	};

} // namespace graph::query::indexes
