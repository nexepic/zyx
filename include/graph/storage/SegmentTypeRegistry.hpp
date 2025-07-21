/**
 * @file SegmentTypeRegistry.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
		static void registerType(const EntityType type) { instance().typeInfo_[type] = TypeInfo{}; }

		static void setChainHead(const EntityType type, const uint64_t offset) {
			instance().typeInfo_[type].chainHead = offset;
		}

		static uint64_t getChainHead(const EntityType type) {
			return instance().typeInfo_.contains(type) ? instance().typeInfo_[type].chainHead : 0;
		}

		static std::vector<EntityType> getAllTypes() {
			std::vector<EntityType> types;
			for (const auto &type: instance().typeInfo_ | std::views::keys) {
				types.push_back(type);
			}
			return types;
		}

	private:
		struct TypeInfo {
			uint64_t chainHead = 0;
		};

		std::unordered_map<EntityType, TypeInfo> typeInfo_;

		static SegmentTypeRegistry &instance() {
			static SegmentTypeRegistry registry;
			return registry;
		}
	};

} // namespace graph::storage
