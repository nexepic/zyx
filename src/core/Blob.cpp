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

    Blob::Blob(int64_t id, const std::string &data) : id(id), data(data) {}

    int64_t Blob::getId() const { return id; }

    const std::string &Blob::getData() const { return data; }

    size_t Blob::getSize() const { return data.size(); }

    bool Blob::hasTemporaryId() const { return storage::IDAllocator::isTemporaryId(id); }

    void Blob::setPermanentId(int64_t permanentId) {
        if (!hasTemporaryId()) {
            throw std::runtime_error("Cannot set permanent ID for blob that already has one");
        }
        id = permanentId;
    }

    void Blob::markInactive() { isActive_ = false; }

    bool Blob::isActive() const { return isActive_; }

    void Blob::serialize(std::ostream &os) const {
        // Write fixed-size blob data
        utils::Serializer::writePOD(os, id);
        utils::Serializer::writePOD(os, isActive_);
        utils::Serializer::writePOD(os, entityId);
        utils::Serializer::writePOD(os, entityType);

        // Write blob chain fields
        utils::Serializer::writePOD(os, nextBlobId);
        utils::Serializer::writePOD(os, prevBlobId);
        utils::Serializer::writePOD(os, chainPosition);

        // Write compression info
        utils::Serializer::writePOD(os, originalSize);
        utils::Serializer::writePOD(os, compressed);

        // Write blob data
        utils::Serializer::writeString(os, data);
    }

    Blob Blob::deserialize(std::istream &is) {
        auto id = utils::Serializer::readPOD<int64_t>(is);

        Blob blob(id, "");
        blob.isActive_ = utils::Serializer::readPOD<bool>(is);
        blob.entityId = utils::Serializer::readPOD<int64_t>(is);
        blob.entityType = utils::Serializer::readPOD<uint32_t>(is);

        // Read blob chain fields
        blob.nextBlobId = utils::Serializer::readPOD<int64_t>(is);
        blob.prevBlobId = utils::Serializer::readPOD<int64_t>(is);
        blob.chainPosition = utils::Serializer::readPOD<int32_t>(is);

        // Read compression info
        blob.originalSize = utils::Serializer::readPOD<uint32_t>(is);
        blob.compressed = utils::Serializer::readPOD<bool>(is);

        // Read blob data
        std::string data = utils::Serializer::readString(is);
        blob.data = data;

        return blob;
    }

} // namespace graph