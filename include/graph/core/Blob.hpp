/**
 * @file Blob.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include "Types.hpp"

namespace graph {

	class Blob {
	public:
		struct Metadata {
			int64_t id = 0;
			int64_t entityId = 0;
			int64_t nextBlobId = 0;
			int64_t prevBlobId = 0;
			uint32_t dataSize = 0;
			uint32_t entityType = 0;
			uint32_t originalSize = 0;
			int32_t chainPosition = 0;
			bool isActive = true;
			bool compressed = false;
		};

		static constexpr size_t TOTAL_BLOB_SIZE = 256;
		static constexpr size_t METADATA_SIZE = sizeof(Metadata);
		static constexpr size_t CHUNK_SIZE = TOTAL_BLOB_SIZE - METADATA_SIZE;

		static constexpr uint32_t typeId = toUnderlying(EntityType::Blob);
		static constexpr uint32_t MAX_COMPRESSED_SIZE = 5 * 1024 * 1024; // 5MB

		[[nodiscard]] size_t getSerializedSize() const;

		static constexpr size_t getTotalSize() { return TOTAL_BLOB_SIZE; }

		Blob(int64_t id, const std::string &data);
		Blob() = default;

		const Metadata &getMetadata() const { return metadata; }
		Metadata &getMutableMetadata() { return metadata; }

		int64_t getId() const { return metadata.id; }
		void setId(int64_t newId) { metadata.id = newId; }

		const char *getData() const { return dataBuffer; }
		std::string getDataAsString() const;
		uint32_t getSize() const { return metadata.dataSize; } // Changed return type to uint32_t

		bool hasTemporaryId() const;
		void setPermanentId(int64_t permanentId);

		void markInactive() { metadata.isActive = false; }
		bool isActive() const { return metadata.isActive; }

		int64_t getNextBlobId() const { return metadata.nextBlobId; }
		int64_t getPrevBlobId() const { return metadata.prevBlobId; }
		void setNextBlobId(int64_t id) { metadata.nextBlobId = id; }
		void setPrevBlobId(int64_t id) { metadata.prevBlobId = id; }
		bool isChained() const { return metadata.nextBlobId != 0; }
		bool isChainStart() const { return metadata.prevBlobId == 0 && metadata.nextBlobId != 0; }
		bool isChainEnd() const { return metadata.nextBlobId == 0 && metadata.prevBlobId != 0; }
		void setChainPosition(int32_t pos) { metadata.chainPosition = pos; }
		int32_t getChainPosition() const { return metadata.chainPosition; }

		void serialize(std::ostream &os) const;
		static Blob deserialize(std::istream &is);

		void setEntityId(int64_t newEntityId) { metadata.entityId = newEntityId; }
		void setEntityType(uint32_t newEntityType) { metadata.entityType = newEntityType; }
		[[nodiscard]] int64_t getEntityId() const { return metadata.entityId; }
		[[nodiscard]] uint32_t getEntityType() const { return metadata.entityType; }

		void setEntityInfo(int64_t newEntityId, uint32_t newEntityType) {
			metadata.entityId = newEntityId;
			metadata.entityType = newEntityType;
		}

		void setData(const std::string &newData);
		static bool canFitData(uint32_t dataSize); // Changed parameter type to uint32_t

		void setCompressionInfo(uint32_t origSize, bool isCompressed) {
			metadata.originalSize = origSize;
			metadata.compressed = isCompressed;
		}

		[[nodiscard]] uint32_t getOriginalSize() const { return metadata.originalSize; }
		[[nodiscard]] bool isCompressed() const { return metadata.compressed; }

	private:
		Metadata metadata;
		char dataBuffer[CHUNK_SIZE] = {};
	};

} // namespace graph
