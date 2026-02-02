/**
 * @file BlobManager.cpp
 * @author Nexepic
 * @date 2025/7/24
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

#include <utility>

#include "graph/core/BlobChainManager.hpp"
#include "graph/storage/data/BlobManager.hpp"

namespace graph::storage {

	BlobManager::BlobManager(const std::shared_ptr<DataManager> &dataManager,
							 std::shared_ptr<BlobChainManager> blobChainManager,
							 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)), blobChainManager_(std::move(blobChainManager)) {}

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

	std::vector<Blob> BlobManager::updateBlobChain(int64_t headBlobId, int64_t entityId, uint32_t entityType,
												   const std::string &data) const {
		// Delegate to BlobChainManager
		return blobChainManager_->updateBlobChain(headBlobId, entityId, entityType, data);
	}

	void BlobManager::deleteBlobChain(int64_t headBlobId) const {
		// Delegate to BlobChainManager
		blobChainManager_->deleteBlobChain(headBlobId);
	}

	int64_t BlobManager::doAllocateId() {
		return getDataManagerPtr()->getIdAllocator()->allocateId(Blob::typeId);
	}

} // namespace graph::storage
