/**
 * @file EntityPropertyTraits.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
		static bool hasPropertyEntity(const Property &entity) { return false; }
		static PropertyStorageType getPropertyStorageType(const Property &entity) { return PropertyStorageType::NONE; }
		static int64_t getPropertyEntityId(const Property &entity) { return 0; }
		static void setPropertyEntityId(Property &entity, int64_t id, PropertyStorageType type) {}
		static bool hasProperty(const Property &entity, const std::string &key) { return false; }
		static void removeProperty(Property &entity, const std::string &key) {}
		static void addProperty(Property &entity, const std::string &key, const PropertyValue &value) {
			throw std::runtime_error("Property entities do not support adding properties");
		}
		static void clearProperties(Property &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties(const Property &entity) { return {}; }
	};

	// Specialization for Blob - doesn't support properties
	template<>
	struct EntityPropertyTraits<Blob> {
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;

		static bool hasPropertyEntity(const Blob &entity) { return false; }
		static PropertyStorageType getPropertyStorageType(const Blob &entity) { return PropertyStorageType::NONE; }
		static int64_t getPropertyEntityId(const Blob &entity) { return 0; }
		static void setPropertyEntityId(Blob &entity, int64_t id, PropertyStorageType type) {}
		static bool hasProperty(const Blob &entity, const std::string &key) { return false; }
		static void removeProperty(Blob &entity, const std::string &key) {}
		static void addProperty(Blob &entity, const std::string &key, const PropertyValue &value) {
			throw std::runtime_error("Blob entities do not support adding properties");
		}
		static void clearProperties(Blob &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties(const Blob &entity) { return {}; }
	};

	// Specialization for Index - doesn't support properties
	template<>
	struct EntityPropertyTraits<Index> {
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;

		static bool hasPropertyEntity(const Index &entity) { return false; }
		static PropertyStorageType getPropertyStorageType(const Index &entity) { return PropertyStorageType::NONE; }
		static int64_t getPropertyEntityId(const Index &entity) { return 0; }
		static void setPropertyEntityId(Index &entity, int64_t id, PropertyStorageType type) {}
		static bool hasProperty(const Index &entity, const std::string &key) { return false; }
		static void removeProperty(Index &entity, const std::string &key) {}
		static void addProperty(Index &entity, const std::string &key, const PropertyValue &value) {
			throw std::runtime_error("Index entities do not support adding properties");
		}
		static void clearProperties(Index &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties(const Index &entity) { return {}; }
	};

	// Specialization for State - doesn't support properties
	template<>
	struct EntityPropertyTraits<State> {
		static constexpr bool supportsProperties = false;
		static constexpr bool supportsExternalProperties = false;

		static bool hasPropertyEntity(const State &entity) { return false; }
		static PropertyStorageType getPropertyStorageType(const State &entity) { return PropertyStorageType::NONE; }
		static int64_t getPropertyEntityId(const State &entity) { return 0; }
		static void setPropertyEntityId(State &entity, int64_t id, PropertyStorageType type) {}
		static bool hasProperty(const State &entity, const std::string &key) { return false; }
		static void removeProperty(State &entity, const std::string &key) {}
		static void addProperty(State &entity, const std::string &key, const PropertyValue &value) {
			throw std::runtime_error("State entities do not support adding properties");
		}
		static void clearProperties(State &entity) {}
		static std::unordered_map<std::string, PropertyValue> getProperties(const State &entity) { return {}; }
	};

} // namespace graph::storage
