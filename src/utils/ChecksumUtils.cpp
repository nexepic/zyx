/**
 * @file ChecksumUtils.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/utils/ChecksumUtils.h"
#include <cstdint>
#include <zlib.h>

namespace graph::utils {

	uint32_t calculateCrc(const void *data, size_t length) {
		return crc32(0L, static_cast<const Bytef *>(data), length);
	}

} // namespace graph::utils