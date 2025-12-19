/**
 * @file SystemStateManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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

	// --- Template Implementations (Must be in header or explicitly instantiated) ---
	// For simplicity in this example, we implement the logic here but usually
	// templates stay in .hpp. To keep .hpp clean, we can put these in the .cpp
	// IF we explicitly instantiate them at the bottom.

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
		if (map.empty()) {
			if (mode == UpdateMode::REPLACE) {
				remove(stateKey);
			}
			return;
		}

		std::unordered_map<std::string, PropertyValue> props;

		// [FIX] MERGE logic: Load existing data first
		if (mode == UpdateMode::MERGE) {
			props = dataManager_->getStateProperties(stateKey);
		} else {
			// REPLACE logic: Explicitly remove old state to ensure clean slate
			// (Optional if updateStateChain handles full overwrite, but safe to keep)
			dataManager_->removeState(stateKey);
		}

		// Apply updates
		for (const auto &[k, v]: map) {
			props[k] = v;
		}

		// Write full set
		dataManager_->addStateProperties(stateKey, props);
	}

	template std::unordered_map<std::string, std::string>
	SystemStateManager::getMap<std::string>(const std::string &) const;
	template void SystemStateManager::setMap<std::string>(const std::string &,
														  const std::unordered_map<std::string, std::string> &,
														  UpdateMode);

	// Explicit Instantiations for the types we support
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

	// double (This was missing!)
	template double SystemStateManager::get<double>(const std::string &, const std::string &, double) const;
	template void SystemStateManager::set<double>(const std::string &, const std::string &, const double &);

} // namespace graph::storage::state
