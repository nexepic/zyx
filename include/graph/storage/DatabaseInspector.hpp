/**
 * @file DatabaseInspector.hpp
 * @author Nexepic
 * @date 2025/3/31
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
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage {

	class DatabaseInspector {
	public:
		explicit DatabaseInspector(const FileHeader &fileHeader, std::shared_ptr<std::fstream> file,
								   DataManager &dataManager);
		~DatabaseInspector();

		/**
		 * @brief Displays the Global File Header and segment usage statistics.
		 */
		void inspectSummary() const;

		/**
		 * @brief Inspects a SINGLE Node segment page.
		 * @param pageIndex The 0-based index of the segment in the linked list chain.
		 * @param showUnused If true, displays empty/deleted slots.
		 */
		void inspectNodeSegments(uint32_t pageIndex, bool showUnused = false) const;

		/**
		 * @brief Inspects a SINGLE Edge segment page.
		 */
		void inspectEdgeSegments(uint32_t pageIndex, bool showUnused = false) const;

		/**
		 * @brief Inspects a SINGLE Property segment page.
		 */
		void inspectPropertySegments(uint32_t pageIndex, bool showUnused = false) const;

		/**
		 * @brief Inspects a SINGLE Blob segment page.
		 */
		void inspectBlobSegments(uint32_t pageIndex, bool showUnused = false) const;

		/**
		 * @brief Inspects a SINGLE Index segment page.
		 */
		void inspectIndexSegments(uint32_t pageIndex, bool showUnused = false) const;

		/**
		 * @brief Inspects a SINGLE State segment page.
		 */
		void inspectStateSegments(uint32_t pageIndex, bool showUnused = false) const;

		/**
		 * @brief Inspects the logical content of a specific State by its Key.
		 * This reassembles the chain and deserializes properties.
		 * @param stateKey The unique key of the state (e.g., "sys.config").
		 */
		void inspectStateData(const std::string &stateKey) const;

	private:
		const FileHeader &fileHeader;
		std::shared_ptr<std::fstream> file;
		DataManager &dataManager_;

		/**
		 * @brief Traverses the linked list of segments to find the physical offset of the Nth segment.
		 * @param headOffset The starting offset from the FileHeader.
		 * @param targetPage The index of the segment to find (0 = head).
		 * @return The file offset of the segment, or 0 if not found/out of bounds.
		 */
		[[nodiscard]] uint64_t navigateToSegment(uint64_t headOffset, uint32_t targetPage) const;

		// Helper to format property maps into a short string "{k:v, ...}"
		static std::string formatPropertiesCompact(const std::unordered_map<std::string, PropertyValue> &props);
	};

} // namespace graph::storage
