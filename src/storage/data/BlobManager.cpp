/**
 * @file BlobManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/BlobManager.hpp"
#include "graph/core/BlobChainManager.hpp"

namespace graph::storage {

	BlobManager::BlobManager(std::shared_ptr<DataManager> dataManager, std::shared_ptr<PropertyManager> propertyManager,
							 std::shared_ptr<graph::BlobChainManager> blobChainManager,
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager<Blob>(dataManager, propertyManager, deletionManager), blobChainManager_(blobChainManager) {}

	void BlobManager::doRemove(Blob &blob) { deletionManager_->deleteBlob(blob); }

	std::string BlobManager::readBlobChain(int64_t headBlobId) {
		// Delegate to BlobChainManager
		return blobChainManager_->readBlobChain(headBlobId);
	}

	std::vector<Blob> BlobManager::createBlobChain(int64_t entityId, uint32_t entityType, const std::string &data) {
		// Delegate to BlobChainManager
		return blobChainManager_->createBlobChain(entityId, entityType, data);
	}

	void BlobManager::deleteBlobChain(int64_t headBlobId) {
		// Delegate to BlobChainManager
		blobChainManager_->deleteBlobChain(headBlobId);
	}

	int64_t BlobManager::doReserveTemporaryId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return 0;

		return dataManager->reserveTemporaryBlobId();
	}

} // namespace graph::storage
