/**
 * @file BlobManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <vector>
#include "graph/core/Blob.hpp"
#include "graph/storage/data/BaseEntityManager.hpp"

namespace graph {
	class BlobChainManager; // Forward declaration
}

namespace graph::storage {

	class BlobManager : public BaseEntityManager<Blob> {
	public:
		BlobManager(std::shared_ptr<DataManager> dataManager, std::shared_ptr<PropertyManager> propertyManager,
					std::shared_ptr<BlobChainManager> blobChainManager,
					std::shared_ptr<DeletionManager> deletionManager);

		// Blob-specific methods that delegate to BlobChainManager
		std::string readBlobChain(int64_t headBlobId);
		std::vector<Blob> createBlobChain(int64_t entityId, uint32_t entityType, const std::string &data);
		void deleteBlobChain(int64_t headBlobId);

	protected:
		// Implement the abstract method for reserving IDs
		int64_t doReserveTemporaryId() override;

		void doRemove(Blob &blob) override;

	private:
		std::shared_ptr<graph::BlobChainManager> blobChainManager_;
	};

} // namespace graph::storage
