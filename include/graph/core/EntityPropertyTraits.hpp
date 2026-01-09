/**
 * @file EntityPropertyTraits.hpp
 * @author Nexepic
 * @date 2025/7/25
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

#include "Blob.hpp"
#include "Edge.hpp"
#include "Index.hpp"
#include "Node.hpp"
#include "Property.hpp"
#include "State.hpp"

namespace graph::storage {

	/**
	 * Traits class to determine property capabilities of different entity types
	 */
	template<typename EntityType>
	struct EntityPropertyTraits {
		// Default - entities don't support properties
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;
	};

	// Specialization for Node
	template<>
	struct EntityPropertyTraits<Node> {
		static constexpr bool supportsProperties = true;
		static constexpr bool supportsExternalProperties = true;

		static bool hasPropertyEntity(const Node &entity) { return entity.hasPropertyEntity(); }

		static PropertyStorageType getPropertyStorageType(const Node &entity) {
			return entity.getPropertyStorageType();
		}

		static int64_t getPropertyEntityId(const Node &entity) { return entity.getPropertyEntityId(); }

		static void setPropertyEntityId(Node &entity, int64_t id, PropertyStorageType type) {
			entity.setPropertyEntityId(id, type);
		}

		static bool hasProperty(const Node &entity, const std::string &key) { return entity.hasProperty(key); }

		static void removeProperty(Node &entity, const std::string &key) { entity.removeProperty(key); }

		static void addProperty(Node &entity, const std::string &key, const PropertyValue &value) {
			entity.addProperty(key, value);
		}

		static void clearProperties(Node &entity) { entity.clearProperties(); }

		static std::unordered_map<std::string, PropertyValue> getProperties(const Node &entity) {
			return entity.getProperties();
		}
	};

	// Specialization for Edge - similar to Node
	template<>
	struct EntityPropertyTraits<Edge> {
		static constexpr bool supportsProperties = true;
		static constexpr bool supportsExternalProperties = true;

		static bool hasPropertyEntity(const Edge &entity) { return entity.hasPropertyEntity(); }

		static PropertyStorageType getPropertyStorageType(const Edge &entity) {
			return entity.getPropertyStorageType();
		}

		static int64_t getPropertyEntityId(const Edge &entity) { return entity.getPropertyEntityId(); }

		static void setPropertyEntityId(Edge &entity, int64_t id, PropertyStorageType type) {
			entity.setPropertyEntityId(id, type);
		}

		static bool hasProperty(const Edge &entity, const std::string &key) { return entity.hasProperty(key); }

		static void removeProperty(Edge &entity, const std::string &key) { entity.removeProperty(key); }

		static void addProperty(Edge &entity, const std::string &key, const PropertyValue &value) {
			entity.addProperty(key, value);
		}

		static void clearProperties(Edge &entity) { entity.clearProperties(); }

		static std::unordered_map<std::string, PropertyValue> getProperties(const Edge &entity) {
			return entity.getProperties();
		}
	};

	// Specialization for Property - doesn't support properties
	template<>
	struct EntityPropertyTraits<Property> {
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;

		// Provide empty implementations to avoid compilation errors
		static bool hasPropertyEntity([[maybe_unused]] const Property &entity) { return false; }
		static PropertyStorageType getPropertyStorageType([[maybe_unused]] const Property &entity) {
			return PropertyStorageType::NONE;
		}
		static int64_t getPropertyEntityId([[maybe_unused]] const Property &entity) { return 0; }
		static void setPropertyEntityId([[maybe_unused]] Property &entity, [[maybe_unused]] int64_t id,
										[[maybe_unused]] PropertyStorageType type) {}
		static bool hasProperty([[maybe_unused]] const Property &entity, [[maybe_unused]] const std::string &key) {
			return false;
		}
		static void removeProperty([[maybe_unused]] Property &entity, [[maybe_unused]] const std::string &key) {}
		static void addProperty([[maybe_unused]] Property &entity, [[maybe_unused]] const std::string &key,
								[[maybe_unused]] const PropertyValue &value) {
			throw std::runtime_error("Property entities do not support adding properties");
		}
		static void clearProperties([[maybe_unused]] Property &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties([[maybe_unused]] const Property &entity) {
			return {};
		}
	};

	// Specialization for Blob - doesn't support properties
	template<>
	struct EntityPropertyTraits<Blob> {
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;

		static bool hasPropertyEntity([[maybe_unused]] const Blob &entity) { return false; }
		static PropertyStorageType getPropertyStorageType([[maybe_unused]] const Blob &entity) {
			return PropertyStorageType::NONE;
		}
		static int64_t getPropertyEntityId([[maybe_unused]] const Blob &entity) { return 0; }
		static void setPropertyEntityId([[maybe_unused]] Blob &entity, [[maybe_unused]] int64_t id,
										[[maybe_unused]] PropertyStorageType type) {}
		static bool hasProperty([[maybe_unused]] const Blob &entity, [[maybe_unused]] const std::string &key) {
			return false;
		}
		static void removeProperty([[maybe_unused]] Blob &entity, [[maybe_unused]] const std::string &key) {}
		static void addProperty([[maybe_unused]] Blob &entity, [[maybe_unused]] const std::string &key,
								[[maybe_unused]] const PropertyValue &value) {
			throw std::runtime_error("Blob entities do not support adding properties");
		}
		static void clearProperties([[maybe_unused]] Blob &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties([[maybe_unused]] const Blob &entity) {
			return {};
		}
	};

	// Specialization for Index - doesn't support properties
	template<>
	struct EntityPropertyTraits<Index> {
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;

		static bool hasPropertyEntity([[maybe_unused]] const Index &entity) { return false; }
		static PropertyStorageType getPropertyStorageType([[maybe_unused]] const Index &entity) {
			return PropertyStorageType::NONE;
		}
		static int64_t getPropertyEntityId([[maybe_unused]] const Index &entity) { return 0; }
		static void setPropertyEntityId([[maybe_unused]] Index &entity, [[maybe_unused]] int64_t id,
										[[maybe_unused]] PropertyStorageType type) {}
		static bool hasProperty([[maybe_unused]] const Index &entity, [[maybe_unused]] const std::string &key) {
			return false;
		}
		static void removeProperty([[maybe_unused]] Index &entity, [[maybe_unused]] const std::string &key) {}
		static void addProperty([[maybe_unused]] Index &entity, [[maybe_unused]] const std::string &key,
								[[maybe_unused]] const PropertyValue &value) {
			throw std::runtime_error("Index entities do not support adding properties");
		}
		static void clearProperties([[maybe_unused]] Index &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties([[maybe_unused]] const Index &entity) {
			return {};
		}
	};

	// Specialization for State - doesn't support properties
	template<>
	struct EntityPropertyTraits<State> {
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;

		static bool hasPropertyEntity([[maybe_unused]] const State &entity) { return false; }
		static PropertyStorageType getPropertyStorageType([[maybe_unused]] const State &entity) {
			return PropertyStorageType::NONE;
		}
		static int64_t getPropertyEntityId([[maybe_unused]] const State &entity) { return 0; }
		static void setPropertyEntityId([[maybe_unused]] State &entity, [[maybe_unused]] int64_t id,
										[[maybe_unused]] PropertyStorageType type) {}
		static bool hasProperty([[maybe_unused]] const State &entity, [[maybe_unused]] const std::string &key) {
			return false;
		}
		static void removeProperty([[maybe_unused]] State &entity, [[maybe_unused]] const std::string &key) {}
		static void addProperty([[maybe_unused]] State &entity, [[maybe_unused]] const std::string &key,
								[[maybe_unused]] const PropertyValue &value) {
			throw std::runtime_error("State entities do not support adding properties");
		}
		static void clearProperties([[maybe_unused]] State &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties([[maybe_unused]] const State &entity) {
			return {};
		}
	};

} // namespace graph::storage
