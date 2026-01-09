/**
 * @file Types.hpp
 * @author Nexepic
 * @date 2025/3/18
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

#pragma once

#include <cstdint>

namespace graph {

	enum class EntityType : uint32_t { Node, Edge, Property, Blob, Index, State };

	constexpr uint32_t toUnderlying(EntityType type) { return static_cast<uint32_t>(type); }

	constexpr uint32_t getMaxEntityType() { return static_cast<uint32_t>(EntityType::State); }

	enum class PropertyStorageType : uint32_t { NONE, PROPERTY_ENTITY, BLOB_ENTITY };

} // namespace graph
