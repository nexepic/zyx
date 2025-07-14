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

#include <string>
#include "Types.hpp"
#include "graph/core/Entity.hpp"

namespace graph {

	class Blob : public EntityBase<Blob>,
				 public ChainableMixin<Blob>,
				 public EntityReferenceMixin<Blob>,
				 public CompressibleMixin<Blob> {
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

		// Accessor methods for metadata and CRTP base classes
		const Metadata &getMetadata() const { return metadata; }
		Metadata &getMutableMetadata() { return metadata; }

		// Adapter methods for ChainableMixin
		int64_t getNextChainId() const { return metadata.nextBlobId; }
		int64_t getPrevChainId() const { return metadata.prevBlobId; }
		void setNextChainId(int64_t id) { metadata.nextBlobId = id; }
		void setPrevChainId(int64_t id) { metadata.prevBlobId = id; }

		// Legacy methods for backward compatibility
		int64_t getNextBlobId() const { return getNextId(); }
		int64_t getPrevBlobId() const { return getPrevId(); }
		void setNextBlobId(int64_t id) { setNextId(id); }
		void setPrevBlobId(int64_t id) { setPrevId(id); }

		// Data methods
		const char *getData() const { return dataBuffer; }
		std::string getDataAsString() const;
		uint32_t getSize() const { return metadata.dataSize; }
		void setData(const std::string &newData);
		static bool canFitData(uint32_t dataSize);

		// Serialization
		void serialize(std::ostream &os) const;
		static Blob deserialize(std::istream &is);

	private:
		Metadata metadata;
		char dataBuffer[CHUNK_SIZE] = {};
	};

} // namespace graph
