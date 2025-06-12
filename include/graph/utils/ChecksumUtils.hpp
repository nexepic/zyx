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
#include "graph/core/Edge.hpp"

namespace graph::utils {

    uint32_t calculateCrc(const void *data, size_t length);

} // namespace graph::utils