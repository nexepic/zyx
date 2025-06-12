/**
 * @file CompressUtils.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <string>
#include <cstdint>

namespace graph::utils {

	std::string zlibCompress(const std::string& data);

	std::string zlibDecompress(const std::string& data, uint32_t originalSize);

} // namespace graph::utils