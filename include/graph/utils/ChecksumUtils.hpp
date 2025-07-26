/**
 * @file ChecksumUtils.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <cstddef>

namespace graph::utils {

	uint32_t calculateCrc(const void *data, size_t length);

	uint32_t updateCrc(uint32_t initialCrc, const void *data, size_t length);

} // namespace graph::utils
