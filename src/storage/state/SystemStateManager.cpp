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

#include "graph/storage/data/StateManager.hpp"

namespace graph::storage::state {

	SystemStateManager::SystemStateManager(std::shared_ptr<DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	std::unordered_map<std::string, PropertyValue> SystemStateManager::getAll(const std::string &stateKey) const {
		// Delegate to DataManager to deserialize the whole chain
		return dataManager_->getStateProperties(stateKey);
	}

	void SystemStateManager::remove(const std::string &stateKey) const { dataManager_->removeState(stateKey); }

	// --- Template Implementations ---

	// Helper to convert std::vector<float> to PropertyValue (for vector index config)
	inline PropertyValue toPropertyValue(const std::vector<float>& vec) {
		std::vector<PropertyValue> propVec;
		propVec.reserve(vec.size());
		for (float f : vec) {
			propVec.push_back(PropertyValue(static_cast<double>(f)));
		}
		return PropertyValue(propVec);
	}

	// Overload for std::vector<float> to handle conversion to PropertyValue
	template<>
	void SystemStateManager::set<std::vector<float>>(const std::string &stateKey, const std::string &field, const std::vector<float> &value, bool useBlobStorage) {
		std::unordered_map<std::string, PropertyValue> singleMap;
		singleMap[field] = toPropertyValue(value);
		setMap(stateKey, singleMap, UpdateMode::MERGE, useBlobStorage);
	}

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

	// Specialization for std::vector<float> to extract from std::vector<PropertyValue>
	// This is needed for vector index embeddings which should stay as float vectors
	template<>
	std::optional<std::vector<float>> extract<std::vector<float>>(const PropertyValue &pv) {
		if (const auto *val = std::get_if<std::vector<PropertyValue>>(&pv.getVariant())) {
			std::vector<float> result;
			result.reserve(val->size());
			for (const auto &elem : *val) {
				if (elem.getType() == PropertyType::DOUBLE) {
					result.push_back(static_cast<float>(std::get<double>(elem.getVariant())));
				} else if (elem.getType() == PropertyType::INTEGER) {
					result.push_back(static_cast<float>(std::get<int64_t>(elem.getVariant())));
				} else {
					// Non-numeric element in vector - return nullopt
					return std::nullopt;
				}
			}
			return result;
		}
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
	void SystemStateManager::set(const std::string &stateKey, const std::string &field, const T &value,
								 bool useBlobStorage) {
		std::unordered_map<std::string, T> singleMap;
		singleMap[field] = value;
		setMap(stateKey, singleMap, UpdateMode::MERGE, useBlobStorage);
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
									UpdateMode mode, bool useBlobStorage) {
		// 1. Handle Empty Input Map
		if (map.empty()) {
			if (mode == UpdateMode::REPLACE) {
				if (!dataManager_->getStateProperties(stateKey).empty()) {
					remove(stateKey);
				}
			}
			return;
		}

		// 2. Load existing data
		auto currentProps = dataManager_->getStateProperties(stateKey);
		bool isDirty = false;

		// 3. Logic based on Mode
		if (mode == UpdateMode::MERGE) {
			for (const auto &[key, value]: map) {
				// Handle std::vector<float> specially by converting to PropertyValue
				PropertyValue newVal;
				if constexpr (std::is_same_v<T, std::vector<float>>) {
					newVal = toPropertyValue(value);
				} else {
					newVal = PropertyValue(value);
				}
				auto it = currentProps.find(key);
				if (it == currentProps.end() || it->second != newVal) {
					currentProps[key] = newVal;
					isDirty = true;
				}
			}
		} else { // REPLACE
			if (currentProps.size() != map.size()) {
				isDirty = true;
			} else {
				for (const auto &[key, value]: map) {
					// Handle std::vector<float> specially by converting to PropertyValue
					PropertyValue compareVal;
					if constexpr (std::is_same_v<T, std::vector<float>>) {
						compareVal = toPropertyValue(value);
					} else {
						compareVal = PropertyValue(value);
					}
					auto it = currentProps.find(key);
					if (it == currentProps.end() || it->second != compareVal) {
						isDirty = true;
						break;
					}
				}
			}
			if (isDirty) {
				currentProps.clear();
				for (const auto &[key, value]: map) {
					// Handle std::vector<float> specially by converting to PropertyValue
					if constexpr (std::is_same_v<T, std::vector<float>>) {
						currentProps[key] = toPropertyValue(value);
					} else {
						currentProps[key] = PropertyValue(value);
					}
				}
				// Explicit remove not strictly necessary if update handles it,
				// but keeps logic clean for full replace
				dataManager_->removeState(stateKey);
			}
		}

		// 4. Write back
		if (isDirty) {
			// Call the updated addStateProperties with useBlobStorage
			dataManager_->addStateProperties(stateKey, currentProps, useBlobStorage);
		}
	}

	// --- Explicit Instantiations ---

	// Float Vector (std::vector<float>) - Required for Vector Index config
	using FloatVec = std::vector<float>;
	template FloatVec SystemStateManager::get<FloatVec>(const std::string &, const std::string &, FloatVec) const;
	template void SystemStateManager::set<FloatVec>(const std::string &, const std::string &, const FloatVec &, bool);
	template std::unordered_map<std::string, FloatVec> SystemStateManager::getMap<FloatVec>(const std::string &) const;
	template void SystemStateManager::setMap<FloatVec>(const std::string &,
													   const std::unordered_map<std::string, FloatVec> &, UpdateMode,
													   bool);

	// PropertyValue itself (Useful for generic storage)
	template void SystemStateManager::setMap<PropertyValue>(const std::string &,
															const std::unordered_map<std::string, PropertyValue> &,
															UpdateMode, bool);

	// std::string
	template std::unordered_map<std::string, std::string>
	SystemStateManager::getMap<std::string>(const std::string &) const;
	template void SystemStateManager::setMap<std::string>(const std::string &,
														  const std::unordered_map<std::string, std::string> &,
														  UpdateMode, bool useBlobStorage);
	template std::string SystemStateManager::get<std::string>(const std::string &, const std::string &,
															  std::string) const;
	template void SystemStateManager::set<std::string>(const std::string &, const std::string &, const std::string &,
													   bool useBlobStorage);

	// int64_t
	template int64_t SystemStateManager::get<int64_t>(const std::string &, const std::string &, int64_t) const;
	template void SystemStateManager::set<int64_t>(const std::string &, const std::string &, const int64_t &,
												   bool useBlobStorage);
	template std::unordered_map<std::string, int64_t> SystemStateManager::getMap<int64_t>(const std::string &) const;
	template void SystemStateManager::setMap<int64_t>(const std::string &,
													  const std::unordered_map<std::string, int64_t> &, UpdateMode,
													  bool useBlobStorage);

	// bool
	template bool SystemStateManager::get<bool>(const std::string &, const std::string &, bool) const;
	template void SystemStateManager::set<bool>(const std::string &, const std::string &, const bool &,
												bool useBlobStorage);

	// double
	template double SystemStateManager::get<double>(const std::string &, const std::string &, double) const;
	template void SystemStateManager::set<double>(const std::string &, const std::string &, const double &,
												  bool useBlobStorage);

} // namespace graph::storage::state
