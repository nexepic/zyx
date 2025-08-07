/**
 * @file EntityTypeIndexManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/4
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "LabelIndex.hpp"
#include "PropertyIndex.hpp"
#include <memory>
#include <string>


namespace graph::storage {
	class DataManager;
}

namespace graph::query::indexes {

	/**
	 * Manages the label and property indexes for a specific type of entity (Node or Edge).
	 */
	class EntityTypeIndexManager {
	public:
		EntityTypeIndexManager(std::shared_ptr<storage::DataManager> dataManager,
							   uint32_t labelIndexType, const std::string& labelStateKey,
							   uint32_t propertyIndexType, const std::string& propertyStateKeyPrefix);

		~EntityTypeIndexManager() = default;

		// --- Index Accessors ---
		std::shared_ptr<LabelIndex> getLabelIndex() const;
		std::shared_ptr<PropertyIndex> getPropertyIndex() const;

		// --- Index Lifecycle ---
		bool buildLabelIndex(const std::function<bool()>& buildFunc);
		bool buildPropertyIndex(const std::string& key, const std::function<bool()>& buildFunc);
		bool buildAllIndexes(const std::function<bool()>& buildFunc);
		bool dropIndex(const std::string& indexType, const std::string& key);
		std::vector<std::pair<std::string, std::string>> listIndexes() const;
		void persistState() const;

		// --- Event Handlers for Automatic Index Updates ---
		template<typename T>
		void onEntityAdded(const T& entity) const;

		template<typename T>
		void onEntityUpdated(const T& oldEntity, const T& newEntity) const;

		template<typename T>
		void onEntityDeleted(const T& entity) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		std::shared_ptr<LabelIndex> labelIndex_;
		std::shared_ptr<PropertyIndex> propertyIndex_;
		mutable std::recursive_mutex mutex_;

		const std::string configLabelEnabledKey_;
		const std::string configPropertyKeysKey_;

		// --- Automatic Update Logic ---
		template<typename T>
		void updateLabelIndex(const T& entity, const std::string& oldLabel, bool isDeleted) const;

		void updatePropertyIndexes(int64_t entityId,
								   const std::unordered_map<std::string, PropertyValue>& oldProps,
								   const std::unordered_map<std::string, PropertyValue>& newProps) const;
	};

} // namespace graph::query::indexes