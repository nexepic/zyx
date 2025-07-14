/**
 * @file Entity.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/14
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Entity.hpp"
#include "graph/storage/IDAllocator.hpp"

namespace graph {

	bool isTemporaryId(int64_t id) { return storage::IDAllocator::isTemporaryId(id); }

} // namespace graph
