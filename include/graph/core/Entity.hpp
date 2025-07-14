/**
 * @file Entity.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/14
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <cstdint>
#include <iostream>

namespace graph {

	bool isTemporaryId(int64_t id);

	/**
	 * Base template for all entity types using CRTP pattern
	 */
	template<typename Derived>
	class EntityBase {
	public:
		// ID management
		int64_t getId() const { return static_cast<const Derived *>(this)->getMetadata().id; }

		void setId(int64_t newId) { static_cast<Derived *>(this)->getMutableMetadata().id = newId; }

		// Active state management
		bool isActive() const { return static_cast<const Derived *>(this)->getMetadata().isActive; }

		void markInactive() { static_cast<Derived *>(this)->getMutableMetadata().isActive = false; }

		// ID type checking
		bool hasTemporaryId() const { return isTemporaryId(getId()); }

		void setPermanentId(int64_t permanentId) {
			if (hasTemporaryId()) {
				setId(permanentId);
			}
		}
	};

	/**
	 * Mixin for entities that form chains (Blob, State)
	 * Uses adapter methods to handle different field names
	 */
	template<typename Derived>
	class ChainableMixin {
	public:
		// These methods rely on Derived class providing these adapters
		int64_t getNextId() const { return static_cast<const Derived *>(this)->getNextChainId(); }

		int64_t getPrevId() const { return static_cast<const Derived *>(this)->getPrevChainId(); }

		void setNextId(int64_t id) { static_cast<Derived *>(this)->setNextChainId(id); }

		void setPrevId(int64_t id) { static_cast<Derived *>(this)->setPrevChainId(id); }

		bool isChained() const { return getNextId() != 0; }

		bool isChainStart() const { return getPrevId() == 0 && getNextId() != 0; }

		bool isChainEnd() const { return getNextId() == 0 && getPrevId() != 0; }

		int32_t getChainPosition() const { return static_cast<const Derived *>(this)->getMetadata().chainPosition; }

		void setChainPosition(int32_t pos) { static_cast<Derived *>(this)->getMutableMetadata().chainPosition = pos; }
	};

	/**
	 * Mixin for entities that reference other entities (Blob, Property)
	 */
	template<typename Derived>
	class EntityReferenceMixin {
	public:
		int64_t getEntityId() const { return static_cast<const Derived *>(this)->getMetadata().entityId; }

		uint32_t getEntityType() const { return static_cast<const Derived *>(this)->getMetadata().entityType; }

		void setEntityId(int64_t newEntityId) {
			static_cast<Derived *>(this)->getMutableMetadata().entityId = newEntityId;
		}

		void setEntityType(uint32_t newEntityType) {
			static_cast<Derived *>(this)->getMutableMetadata().entityType = newEntityType;
		}

		void setEntityInfo(int64_t newEntityId, uint32_t newEntityType) {
			auto &metadata = static_cast<Derived *>(this)->getMutableMetadata();
			metadata.entityId = newEntityId;
			metadata.entityType = newEntityType;
		}
	};

	/**
	 * Mixin for compressed data entities (Blob)
	 */
	template<typename Derived>
	class CompressibleMixin {
	public:
		uint32_t getOriginalSize() const { return static_cast<const Derived *>(this)->getMetadata().originalSize; }

		bool isCompressed() const { return static_cast<const Derived *>(this)->getMetadata().compressed; }

		void setCompressionInfo(uint32_t origSize, bool isCompressed) {
			auto &metadata = static_cast<Derived *>(this)->getMutableMetadata();
			metadata.originalSize = origSize;
			metadata.compressed = isCompressed;
		}
	};

} // namespace graph
