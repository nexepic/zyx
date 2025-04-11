/**
 * @file BlobStore.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/BlobStore.h"
#include <cstring>
#include "graph/storage/StorageHeaders.h"
#include "graph/utils/ChecksumUtils.h"
#include "graph/utils/CompressUtils.h"

namespace graph::storage {

	BlobStore::BlobStore(std::shared_ptr<std::fstream> file, uint64_t blobSectionOffset) :
		file_(std::move(file)), blobSectionOffset_(blobSectionOffset), nextBlobId_(1) {

		if (blobSectionOffset_ != 0) {
			// Read the section header to find the highest blob ID
			BlobSectionHeader sectionHeader;
			uint64_t currentSection = blobSectionOffset_;

			while (currentSection != 0) {
				file_->seekg(static_cast<std::streamoff>(currentSection));
				file_->read(reinterpret_cast<char *>(&sectionHeader), sizeof(BlobSectionHeader));

				// Scan through blobs in this section to find the highest ID
				uint64_t dataOffset = currentSection + sizeof(BlobSectionHeader);
				uint32_t remainingBytes = sectionHeader.used;

				while (remainingBytes > sizeof(BlobEntryHeader)) {
					file_->seekg(static_cast<std::streamoff>(dataOffset));
					BlobEntryHeader blobHeader;
					file_->read(reinterpret_cast<char *>(&blobHeader), sizeof(BlobEntryHeader));

					if (blobHeader.id >= nextBlobId_) {
						nextBlobId_ = blobHeader.id + 1;
					}

					dataOffset += sizeof(BlobEntryHeader) + blobHeader.stored_size;
					remainingBytes -= sizeof(BlobEntryHeader) + blobHeader.stored_size;
				}

				currentSection = sectionHeader.next_section_offset;
			}
		}
	}

	uint64_t BlobStore::storeBlob(const std::string &data, const std::string &contentType) {
		// Check if we need to compress the data
		bool shouldCompress = BlobStore::shouldCompress(data, contentType);
		std::string storedData = shouldCompress ? compressData(data) : data;

		// Prepare the blob entry header
		BlobEntryHeader header;
		header.id = nextBlobId_++;
		header.size = static_cast<uint32_t>(data.size());
		header.stored_size = static_cast<uint32_t>(storedData.size());
		header.compressed = shouldCompress;
		std::strncpy(header.content_type, contentType.c_str(), sizeof(header.content_type) - 1);

		// Calculate CRC
		header.entry_crc = utils::calculateCrc(&header, offsetof(BlobEntryHeader, entry_crc));

		// Find a section with enough space
		uint32_t requiredSpace = sizeof(BlobEntryHeader) + header.stored_size;
		uint64_t currentSection = blobSectionOffset_;
		BlobSectionHeader sectionHeader;

		bool foundSpace = false;
		while (currentSection != 0) {
			file_->seekg(static_cast<std::streamoff>(currentSection));
			file_->read(reinterpret_cast<char *>(&sectionHeader), sizeof(BlobSectionHeader));

			if (sectionHeader.capacity - sectionHeader.used >= requiredSpace) {
				foundSpace = true;
				break;
			}

			currentSection = sectionHeader.next_section_offset;
		}

		// If no section has enough space, allocate a new one
		if (!foundSpace) {
			uint64_t newSectionOffset = allocateSection(std::max(BLOB_SECTION_SIZE, requiredSpace));

			if (blobSectionOffset_ == 0) {
				blobSectionOffset_ = newSectionOffset;
				currentSection = newSectionOffset;
			} else {
				// Link the new section to the chain
				uint64_t lastSection = blobSectionOffset_;
				while (sectionHeader.next_section_offset != 0) {
					lastSection = sectionHeader.next_section_offset;
					file_->seekg(static_cast<std::streamoff>(lastSection));
					file_->read(reinterpret_cast<char *>(&sectionHeader), sizeof(BlobSectionHeader));
				}

				sectionHeader.next_section_offset = newSectionOffset;
				file_->seekp(static_cast<std::streamoff>(lastSection));
				file_->write(reinterpret_cast<const char *>(&sectionHeader), sizeof(BlobSectionHeader));

				currentSection = newSectionOffset;

				// Initialize the new section header
				file_->seekg(static_cast<std::streamoff>(newSectionOffset));
				file_->read(reinterpret_cast<char *>(&sectionHeader), sizeof(BlobSectionHeader));
			}
		}

		// Write the blob entry
		uint64_t blobOffset = currentSection + sizeof(BlobSectionHeader) + sectionHeader.used;

		// Write the header
		file_->seekp(static_cast<std::streamoff>(blobOffset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(BlobEntryHeader));

		// Write the data
		file_->write(storedData.data(), static_cast<std::streamsize>(storedData.size()));

		// Update the section header
		sectionHeader.used += requiredSpace;
		file_->seekp(static_cast<std::streamoff>(currentSection));
		file_->write(reinterpret_cast<const char *>(&sectionHeader), sizeof(BlobSectionHeader));

		// Return the blob ID
		return header.id;
	}

	std::string BlobStore::getBlob(uint64_t id) {
		uint64_t entryOffset = findBlobEntryOffset(id);
		if (entryOffset == 0) {
			throw std::runtime_error("Blob ID " + std::to_string(id) + " not found");
		}

		// Read the blob header
		BlobEntryHeader header;
		file_->seekg(static_cast<std::streamoff>(entryOffset));
		file_->read(reinterpret_cast<char *>(&header), sizeof(BlobEntryHeader));

		// Validate the ID
		if (header.id != id) {
			throw std::runtime_error("Blob ID mismatch, possible data corruption");
		}

		// Read the stored data
		std::string storedData(header.stored_size, '\0');
		file_->read(&storedData[0], static_cast<std::streamsize>(header.stored_size));

		// Verify CRC
		uint32_t calculatedCrc = utils::calculateCrc(&header, offsetof(BlobEntryHeader, entry_crc));
		if (calculatedCrc != header.entry_crc) {
			throw std::runtime_error("Blob CRC mismatch, data corruption detected");
		}

		// Decompress if needed
		if (header.compressed) {
			return decompressData(storedData, header.size);
		}

		return storedData;
	}

	bool BlobStore::shouldCompress(const std::string &data, const std::string &contentType) {
		// Smaller data doesn't benefit much from compression
		if (data.size() < 512) {
			return false;
		}

		// Text data usually compresses well
		if (contentType == "text") {
			return true;
		}

		return false;
	}

	std::string BlobStore::compressData(const std::string &data) { return utils::zlibCompress(data); }

	std::string BlobStore::decompressData(const std::string &data, uint32_t originalSize) {
		return utils::zlibDecompress(data, originalSize);
	}

	uint64_t BlobStore::allocateSection(uint32_t capacity) {
		// Position the file pointer at the end of the file
		file_->seekp(0, std::ios::end);
		uint64_t sectionOffset = file_->tellp();

		// Ensure alignment
		if (sectionOffset % PAGE_SIZE != 0) {
			sectionOffset += PAGE_SIZE - (sectionOffset % PAGE_SIZE);
			file_->seekp(static_cast<std::streamoff>(sectionOffset));
		}

		// Initialize the section header
		BlobSectionHeader header;
		header.capacity = capacity;
		header.used = 0;
		header.next_section_offset = 0;
		header.prev_section_offset = 0;

		// Write the header
		file_->write(reinterpret_cast<const char *>(&header), sizeof(BlobSectionHeader));

		// Pad the rest of the section with zeros
		std::vector<char> padding(capacity, 0);
		file_->write(padding.data(), static_cast<std::streamsize>(capacity));

		return sectionOffset;
	}

	void BlobStore::flush() { file_->flush(); }

	uint64_t BlobStore::findBlobEntryOffset(uint64_t id) {
		if (blobSectionOffset_ == 0) {
			return 0; // No blob sections
		}

		uint64_t currentSection = blobSectionOffset_;
		BlobSectionHeader sectionHeader;

		while (currentSection != 0) {
			file_->seekg(static_cast<std::streamoff>(currentSection));
			file_->read(reinterpret_cast<char *>(&sectionHeader), sizeof(BlobSectionHeader));

			// Scan through blobs in this section
			uint64_t dataOffset = currentSection + sizeof(BlobSectionHeader);
			uint32_t remainingBytes = sectionHeader.used;

			while (remainingBytes > sizeof(BlobEntryHeader)) {
				file_->seekg(static_cast<std::streamoff>(dataOffset));
				BlobEntryHeader blobHeader;
				file_->read(reinterpret_cast<char *>(&blobHeader), sizeof(BlobEntryHeader));

				if (blobHeader.id == id) {
					return dataOffset;
				}

				dataOffset += sizeof(BlobEntryHeader) + blobHeader.stored_size;
				remainingBytes -= sizeof(BlobEntryHeader) + blobHeader.stored_size;
			}

			currentSection = sectionHeader.next_section_offset;
		}

		return 0; // Blob not found
	}

	bool BlobStore::blobExists(uint64_t id) { return findBlobEntryOffset(id) != 0; }

} // namespace graph::storage
