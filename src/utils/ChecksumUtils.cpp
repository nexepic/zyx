/**
 * @file ChecksumUtils.cpp
 * @author Nexepic
 * @date 2025/2/27
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

#include "graph/utils/ChecksumUtils.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <limits>
#include <cstdint>
#include <zlib.h>

namespace graph::utils {

	uint32_t calculateCrc(const void *data, size_t length) {
		debug::ScopedPerfTimer crcTimer("storage.checksum.crc");
		debug::PerfTrace::addValue("storage.checksum.bytes", static_cast<int64_t>(std::min<size_t>(length, std::numeric_limits<int64_t>::max())));
		return crc32(0L, static_cast<const Bytef *>(data), length);
	}

	uint32_t updateCrc(uint32_t initialCrc, const void *data, size_t length) {
		debug::ScopedPerfTimer crcTimer("storage.checksum.crc");
		const auto bytes = static_cast<int64_t>(std::min<size_t>(length, std::numeric_limits<int64_t>::max()));
		debug::PerfTrace::addValue("storage.checksum.bytes", bytes);
		debug::PerfTrace::addValue("storage.checksum.incremental_bytes", bytes);
		return crc32(initialCrc, static_cast<const Bytef *>(data), length);
	}

} // namespace graph::utils
