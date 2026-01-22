/**
 * @file SystemStateManager.hpp
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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage::state {

	// Define update modes
	enum class UpdateMode {
		REPLACE, // Deletes old state first (Default for RootMap consistency)
		MERGE // Appends/Updates existing properties, keeps others
	};

	class SystemStateManager {
	public:
		explicit SystemStateManager(std::shared_ptr<DataManager> dataManager);
		~SystemStateManager() = default;

		// --- Configuration / Mixed Type Access ---

		/**
		 * @brief Reads ALL properties from a state chain as raw PropertyValues.
		 *        Essential for listing configurations which have mixed types.
		 */
		[[nodiscard]] std::unordered_map<std::string, PropertyValue> getAll(const std::string &stateKey) const;

		// --- Generic Scalar Operations ---

		/**
		 * @brief Reads a value from a specific field in a State entity.
		 * @tparam T The expected type (int64_t, bool, double, string).
		 * @param stateKey The unique key of the State chain.
		 * @param field The property name within the State.
		 * @param defaultValue Value to return if key/field missing or type mismatch.
		 */
		template<typename T>
		T get(const std::string &stateKey, const std::string &field, T defaultValue) const;

		/**
		 * @brief Writes a single value to a specific field in a State entity.
		 *        Merges with existing properties in that State.
		 */
		template<typename T>
		void set(const std::string &stateKey, const std::string &field, const T &value, bool useBlobStorage = false);

		// --- Generic Map Operations ---

		/**
		 * @brief Loads all properties from a State entity and converts them to a typed map.
		 *        Useful for loading Root Maps (String -> Int64) or Type Maps (String -> Int).
		 * @tparam T The value type of the map.
		 */
		template<typename T>
		std::unordered_map<std::string, T> getMap(const std::string &stateKey) const;

		/**
		 * @brief Overwrites a State entity with the provided map.
		 */
		template<typename T>
		void setMap(const std::string &stateKey, const std::unordered_map<std::string, T> &map,
					UpdateMode mode = UpdateMode::REPLACE,
					bool useBlobStorage = false); // Default to Replace for safety

		// --- Lifecycle ---

		/**
		 * @brief Completely removes the State entity chain associated with the key.
		 */
		void remove(const std::string &stateKey) const;

	private:
		std::shared_ptr<DataManager> dataManager_;
	};

} // namespace graph::storage::state
