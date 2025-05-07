/**
 * @file Blob.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <iostream>
#include <string>

namespace graph {

	class Blob {
	public:
		static constexpr uint8_t typeId = 3; // Node = 0, Edge = 1, Property = 2, Blob = 3

		Blob(int64_t id, const std::string &data);
		Blob() = default;

		int64_t getId() const;
		void setId(int64_t newId) { id = newId; }
		const std::string &getData() const;
		const std::string &getContentType() const;
		size_t getSize() const;

		bool hasTemporaryId() const;
		void setPermanentId(int64_t permanentId);

		void markInactive();
		bool isActive() const;

		// Serialization methods
		void serialize(std::ostream &os) const;
		static Blob deserialize(std::istream &is);

		void setEntityId(int64_t newEntityId) {
			entityId = newEntityId;
		}

		void setEntityType(uint8_t newEntityType) {
			entityType = newEntityType;
		}

		// setEntityInfo
		void setEntityInfo(int64_t newEntityId, uint8_t newEntityType) {
			entityId = newEntityId;
			entityType = newEntityType;
		}

		// setData
		void setData(const std::string &newData) {
			data = newData;
		}

	private:
		int64_t id = 0;
		int64_t entityId = 0;  // ID of the entity this property belongs to
		uint8_t entityType = 0; // 0 = Node, 1 = Edge
		std::string data;
		bool isActive_ = true;
	};

} // namespace graph
