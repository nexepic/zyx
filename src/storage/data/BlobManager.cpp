/**
 * @file BlobManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <utility>

#include "graph/core/BlobChainManager.hpp"
#include "graph/storage/data/BlobManager.hpp"

namespace graph::storage {

	BlobManager::BlobManager(const std::shared_ptr<DataManager> &dataManager,
							 std::shared_ptr<PropertyManager> propertyManager,
							 std::shared_ptr<BlobChainManager> blobChainManager,
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(propertyManager), std::move(deletionManager)),
		blobChainManager_(std::move(blobChainManager)) {}

	void BlobManager::doRemove(Blob &blob) { deletionManager_->deleteBlob(blob); }

	std::string BlobManager::readBlobChain(int64_t headBlobId) const {
		// Delegate to BlobChainManager
		return blobChainManager_->readBlobChain(headBlobId);
	}

	std::vector<Blob> BlobManager::createBlobChain(int64_t entityId, uint32_t entityType,
												   const std::string &data) const {
		// Delegate to BlobChainManager
		return blobChainManager_->createBlobChain(entityId, entityType, data);
	}

	void BlobManager::deleteBlobChain(int64_t headBlobId) const {
		// Delegate to BlobChainManager
		blobChainManager_->deleteBlobChain(headBlobId);
	}

	int64_t BlobManager::doAllocateId() {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			throw std::runtime_error("DataManager is not available for ID allocation.");
		}
		return dataManager->getIdAllocator()->allocateId(Blob::typeId);
	}

} // namespace graph::storage
