/**
 * @file CompressUtils.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/utils/CompressUtils.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <zlib.h>

namespace graph::utils {

	std::string zlibCompress(const std::string &data) {
		uLongf compressedSize = compressBound(data.size());
		std::string compressedData(compressedSize, '\0');

		const int result = compress(reinterpret_cast<Bytef *>(&compressedData[0]), &compressedSize,
							  reinterpret_cast<const Bytef *>(data.data()), data.size());

		if (result != Z_OK) {
			throw std::runtime_error("Compression failed");
		}

		compressedData.resize(compressedSize);
		return compressedData;
	}

	std::string zlibDecompress(const std::string &data, uint32_t originalSize) {
		std::string decompressedData(originalSize, '\0');
		uLongf decompressedSize = originalSize;

		const int result = uncompress(reinterpret_cast<Bytef *>(&decompressedData[0]), &decompressedSize,
								reinterpret_cast<const Bytef *>(data.data()), data.size());

		if (result != Z_OK) {
			throw std::runtime_error("Decompression failed");
		}

		decompressedData.resize(decompressedSize);
		return decompressedData;
	}

} // namespace graph::utils
