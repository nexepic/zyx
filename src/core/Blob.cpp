/**
 * @file Blob.cpp
 * @author Nexepic
 * @date 2025/4/25
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

#include "graph/core/Blob.hpp"
#include <cstring>
#include "graph/storage/IDAllocator.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

	Blob::Blob(const int64_t id, const std::string &data) {
		metadata.id = id;
		setData(data);
	}

	std::string Blob::getDataAsString() const { return {dataBuffer, metadata.dataSize}; }

	void Blob::setData(const std::string &newData) {
		metadata.dataSize = std::min(newData.size(), CHUNK_SIZE); // Cast to uint32_t
		memcpy(dataBuffer, newData.data(), metadata.dataSize);
	}

	bool Blob::canFitData(uint32_t size) { // Changed parameter type to uint32_t
		return size <= CHUNK_SIZE;
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
