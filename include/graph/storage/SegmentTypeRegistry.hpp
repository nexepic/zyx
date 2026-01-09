/**
 * @file SegmentTypeRegistry.hpp
 * @author Nexepic
 * @date 2025/7/21
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
#include <ranges>
#include <unordered_map>
#include <vector>
#include "graph/core/Types.hpp"

namespace graph::storage {

	/**
	 * Registry for segment types and their chain heads.
	 * Provides methods to register types, set and get chain heads.
	 */
	class SegmentTypeRegistry {
	public:
		SegmentTypeRegistry() = default;
		~SegmentTypeRegistry() = default;

		void registerType(const EntityType type) { typeInfo_[type] = TypeInfo{}; }

		void setChainHead(const EntityType type, const uint64_t offset) { typeInfo_[type].chainHead = offset; }

		[[nodiscard]] uint64_t getChainHead(const EntityType type) const {
			auto it = typeInfo_.find(type);
			return it != typeInfo_.end() ? it->second.chainHead : 0;
		}

		[[nodiscard]] std::vector<EntityType> getAllTypes() const {
			std::vector<EntityType> types;
			types.reserve(typeInfo_.size());
			for (const auto &type: typeInfo_ | std::views::keys) {
				types.push_back(type);
			}
			return types;
		}

		void clear() { typeInfo_.clear(); }

	private:
		struct TypeInfo {
			uint64_t chainHead = 0;
		};

		std::unordered_map<EntityType, TypeInfo> typeInfo_;
	};

} // namespace graph::storage
