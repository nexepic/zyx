/**
 * @file CompressUtils.cpp
 * @author Nexepic
 * @date 2025/3/26
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
