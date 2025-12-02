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

	class BlobManager final : public BaseEntityManager<Blob> {
	public:
		BlobManager(const std::shared_ptr<DataManager> &dataManager, std::shared_ptr<BlobChainManager> blobChainManager,
					std::shared_ptr<DeletionManager> deletionManager);

		// Blob-specific methods that delegate to BlobChainManager
		[[nodiscard]] std::string readBlobChain(int64_t headBlobId) const;
		[[nodiscard]] std::vector<Blob> createBlobChain(int64_t entityId, uint32_t entityType,
														const std::string &data) const;
		[[nodiscard]] std::vector<Blob> updateBlobChain(int64_t headBlobId, int64_t entityId, uint32_t entityType,
														const std::string &data) const;
		void deleteBlobChain(int64_t headBlobId) const;

	protected:
		int64_t doAllocateId() override;

		void doRemove(Blob &blob) override;

	private:
		std::shared_ptr<BlobChainManager> blobChainManager_;
	};

} // namespace graph::storage
