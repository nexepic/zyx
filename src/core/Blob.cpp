/**
 * @file Blob.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Blob.h"
#include <graph/storage/IDAllocator.h>
#include "graph/utils/Serializer.h"

namespace graph {

    Blob::Blob(int64_t id, const std::string &data) {
        metadata.id = id;
        setData(data);
    }

    std::string Blob::getDataAsString() const {
        return {dataBuffer, metadata.dataSize};
    }

    void Blob::setData(const std::string &newData) {
        metadata.dataSize = std::min(newData.size(), CHUNK_SIZE); // Cast to uint32_t
        memcpy(dataBuffer, newData.data(), metadata.dataSize);
    }

    bool Blob::canFitData(uint32_t size) { // Changed parameter type to uint32_t
        return size <= CHUNK_SIZE;
    }

    bool Blob::hasTemporaryId() const {
        return storage::IDAllocator::isTemporaryId(metadata.id);
    }

    void Blob::setPermanentId(int64_t permanentId) {
        if (!hasTemporaryId()) {
            throw std::runtime_error("Cannot set permanent ID for blob that already has one");
        }
        metadata.id = permanentId;
    }

    void Blob::serialize(std::ostream &os) const {
        utils::Serializer::writePOD(os, metadata.id);
        utils::Serializer::writePOD(os, metadata.isActive);
        utils::Serializer::writePOD(os, metadata.entityId);
        utils::Serializer::writePOD(os, metadata.entityType);
        utils::Serializer::writePOD(os, metadata.nextBlobId);
        utils::Serializer::writePOD(os, metadata.prevBlobId);
        utils::Serializer::writePOD(os, metadata.chainPosition);
        utils::Serializer::writePOD(os, metadata.originalSize);
        utils::Serializer::writePOD(os, metadata.compressed);
        utils::Serializer::writePOD(os, metadata.dataSize);
        if (metadata.dataSize > 0) {
            os.write(dataBuffer, metadata.dataSize);
        }
    }

    Blob Blob::deserialize(std::istream &is) {
        Blob blob;
        blob.metadata.id = utils::Serializer::readPOD<int64_t>(is);
        blob.metadata.isActive = utils::Serializer::readPOD<bool>(is);
        blob.metadata.entityId = utils::Serializer::readPOD<int64_t>(is);
        blob.metadata.entityType = utils::Serializer::readPOD<uint32_t>(is);
        blob.metadata.nextBlobId = utils::Serializer::readPOD<int64_t>(is);
        blob.metadata.prevBlobId = utils::Serializer::readPOD<int64_t>(is);
        blob.metadata.chainPosition = utils::Serializer::readPOD<int32_t>(is);
        blob.metadata.originalSize = utils::Serializer::readPOD<uint32_t>(is);
        blob.metadata.compressed = utils::Serializer::readPOD<bool>(is);
        blob.metadata.dataSize = utils::Serializer::readPOD<uint32_t>(is); // Changed to uint32_t
        if (blob.metadata.dataSize > 0) {
            is.read(blob.dataBuffer, blob.metadata.dataSize);
        }
        return blob;
    }

	size_t Blob::getSerializedSize() const {
    	// Calculate size of all metadata fields
    	size_t size = 0;
    	size += sizeof(metadata.id);
    	size += sizeof(metadata.isActive);
    	size += sizeof(metadata.entityId);
    	size += sizeof(metadata.entityType);
    	size += sizeof(metadata.nextBlobId);
    	size += sizeof(metadata.prevBlobId);
    	size += sizeof(metadata.chainPosition);
    	size += sizeof(metadata.originalSize);
    	size += sizeof(metadata.compressed);
    	size += sizeof(metadata.dataSize);

    	// Add size of the actual data buffer
    	size += metadata.dataSize;

    	return size;
    }

} // namespace graph