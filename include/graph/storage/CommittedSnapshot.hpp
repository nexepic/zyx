/**
 * @file CommittedSnapshot.hpp
 * @date 2026/3/27
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#pragma once

#include <cstdint>
#include <unordered_map>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/data/DirtyEntityInfo.hpp"

namespace graph::storage {

	/**
	 * Immutable snapshot of committed dirty entity state at a point in time.
	 * Read-only transactions hold a shared_ptr to this, ensuring they see
	 * a consistent view even after subsequent writer commits.
	 */
	struct CommittedSnapshot {
		uint64_t snapshotId = 0;

		std::unordered_map<int64_t, DirtyEntityInfo<Node>> nodes;
		std::unordered_map<int64_t, DirtyEntityInfo<Edge>> edges;
		std::unordered_map<int64_t, DirtyEntityInfo<Property>> properties;
		std::unordered_map<int64_t, DirtyEntityInfo<Blob>> blobs;
		std::unordered_map<int64_t, DirtyEntityInfo<Index>> indexes;
		std::unordered_map<int64_t, DirtyEntityInfo<State>> states;
	};

} // namespace graph::storage
