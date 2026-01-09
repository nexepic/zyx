/**
 * @file SystemStateManager.cpp
 * @author Nexepic
 * @date 2025/12/15
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

#include "graph/storage/state/SystemStateManager.hpp"

namespace graph::storage::state {

	SystemStateManager::SystemStateManager(std::shared_ptr<DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	std::unordered_map<std::string, PropertyValue> SystemStateManager::getAll(const std::string &stateKey) const {
		// Delegate to DataManager to deserialize the whole chain
		return dataManager_->getStateProperties(stateKey);
	}

	void SystemStateManager::remove(const std::string &stateKey) const { dataManager_->removeState(stateKey); }

	// --- Template Implementations ---

	// Helper to extract value from PropertyValue
	template<typename T>
	std::optional<T> extract(const PropertyValue &pv) {
		if (const auto *val = std::get_if<T>(&pv.getVariant())) {
			return *val;
		}
		return std::nullopt;
	}

	// Specialization for bool because it might be stored as int64_t (0/1)
	template<>
	std::optional<bool> extract<bool>(const PropertyValue &pv) {
		if (const auto *val = std::get_if<bool>(&pv.getVariant()))
			return *val;
		if (const auto *val = std::get_if<int64_t>(&pv.getVariant()))
			return *val != 0;
		return std::nullopt;
	}

	template<typename T>
	T SystemStateManager::get(const std::string &stateKey, const std::string &field, T defaultValue) const {
		auto props = dataManager_->getStateProperties(stateKey);
		if (auto it = props.find(field); it != props.end()) {
			auto val = extract<T>(it->second);
			if (val.has_value())
				return *val;
		}
		return defaultValue;
	}

	template<typename T>
	void SystemStateManager::set(const std::string &stateKey, const std::string &field, const T &value) {
		std::unordered_map<std::string, T> singleMap;
		singleMap[field] = value;
		// Use MERGE mode. The optimization in setMap will handle the check.
		setMap(stateKey, singleMap, UpdateMode::MERGE);
	}

	template<typename T>
	std::unordered_map<std::string, T> SystemStateManager::getMap(const std::string &stateKey) const {
		std::unordered_map<std::string, T> result;
		auto props = dataManager_->getStateProperties(stateKey);
		for (const auto &[k, v]: props) {
			auto val = extract<T>(v);
			if (val.has_value()) {
				result[k] = *val;
			}
		}
		return result;
	}

	template<typename T>
	void SystemStateManager::setMap(const std::string &stateKey, const std::unordered_map<std::string, T> &map,
									UpdateMode mode) {
		// 1. Handle Empty Input Map
		if (map.empty()) {
			// Only remove if we are in REPLACE mode (meaning "set state to empty")
			// If MERGE, empty map means "do nothing".
			if (mode == UpdateMode::REPLACE) {
				// Optimization: Check if it exists before trying to remove
				if (!dataManager_->getStateProperties(stateKey).empty()) {
					remove(stateKey);
				}
			}
			return;
		}

		// 2. Load existing data for comparison and merging
		auto currentProps = dataManager_->getStateProperties(stateKey);
		bool isDirty = false;

		// 3. Logic based on Mode
		if (mode == UpdateMode::MERGE) {
			// Iterate through the input map to check for changes
			for (const auto &[key, value]: map) {
				PropertyValue newVal(value);

				// Update only if:
				// a) The key does not exist in current state
				// b) The key exists but the value is different
				auto it = currentProps.find(key);
				if (it == currentProps.end() || it->second != newVal) {
					currentProps[key] = newVal;
					isDirty = true;
				}
			}
		} else { // UpdateMode::REPLACE
			// In REPLACE mode, the new state must EXACTLY match the input map.

			// Optimization: First check if sizes match.
			if (currentProps.size() != map.size()) {
				isDirty = true;
			} else {
				// Sizes match, check content equality.
				for (const auto &[key, value]: map) {
					auto it = currentProps.find(key);
					// If key missing in old state OR value is different -> Changed
					if (it == currentProps.end() || it->second != PropertyValue(value)) {
						isDirty = true;
						break;
					}
				}
			}

			// If data changed, prepare the new property map from scratch (Replace)
			if (isDirty) {
				// Clean the old map completely so we only write the new map
				currentProps.clear();
				for (const auto &[key, value]: map) {
					currentProps[key] = value;
				}
				// Explicitly remove old state chain to prevent orphan blobs
				// (depending on underlying implementation, this is safer for REPLACE)
				dataManager_->removeState(stateKey);
			}
		}

		// 4. Write back only if changes were detected
		if (isDirty) {
			dataManager_->addStateProperties(stateKey, currentProps);
		}
	}

	// --- Explicit Instantiations ---

	template std::unordered_map<std::string, std::string>
	SystemStateManager::getMap<std::string>(const std::string &) const;
	template void SystemStateManager::setMap<std::string>(const std::string &,
														  const std::unordered_map<std::string, std::string> &,
														  UpdateMode);

	// int64_t
	template int64_t SystemStateManager::get<int64_t>(const std::string &, const std::string &, int64_t) const;
	template void SystemStateManager::set<int64_t>(const std::string &, const std::string &, const int64_t &);
	template std::unordered_map<std::string, int64_t> SystemStateManager::getMap<int64_t>(const std::string &) const;
	template void SystemStateManager::setMap<int64_t>(const std::string &,
													  const std::unordered_map<std::string, int64_t> &, UpdateMode);

	// bool
	template bool SystemStateManager::get<bool>(const std::string &, const std::string &, bool) const;
	template void SystemStateManager::set<bool>(const std::string &, const std::string &, const bool &);

	// std::string
	template std::string SystemStateManager::get<std::string>(const std::string &, const std::string &,
															  std::string) const;
	template void SystemStateManager::set<std::string>(const std::string &, const std::string &, const std::string &);

	// double
	template double SystemStateManager::get<double>(const std::string &, const std::string &, double) const;
	template void SystemStateManager::set<double>(const std::string &, const std::string &, const double &);

} // namespace graph::storage::state
