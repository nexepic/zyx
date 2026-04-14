/**
 * @file UndoLog.hpp
 * @date 2026/4/14
 *
 * @copyright Copyright (c) 2026 Nexepic
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
#include <cstdint>
#include <vector>

namespace graph::storage::wal {

	/**
	 * @brief Change type for an undo log entry.
	 *
	 * Prefixed to avoid Windows macro conflicts (CHANGE_ prefix matches
	 * EntityChangeType naming convention used elsewhere in the codebase).
	 */
	enum class UndoChangeType : uint8_t {
		UNDO_ADDED = 0,    // Entity was added — undo = remove
		UNDO_MODIFIED = 1, // Entity was modified — undo = restore before-image
		UNDO_DELETED = 2   // Entity was deleted — undo = restore before-image as active
	};

	/**
	 * @brief A single undo log entry capturing one mutation.
	 *
	 * beforeImage is empty for UNDO_ADDED (entity did not exist before the op).
	 * For UNDO_MODIFIED and UNDO_DELETED it holds the serialized entity state
	 * that was in effect immediately before the mutation.
	 */
	struct UndoEntry {
		uint8_t entityType;          // graph::EntityType value
		int64_t entityId;
		UndoChangeType changeType;
		std::vector<uint8_t> beforeImage; // Empty for UNDO_ADDED
	};

	/**
	 * @brief In-memory undo log for a single write transaction.
	 *
	 * Entries are appended in operation order and replayed in reverse during
	 * rollback.  The log lives only for the duration of the transaction; it is
	 * cleared on commit or rollback.
	 */
	class UndoLog {
	public:
		void record(UndoEntry entry) { entries_.push_back(std::move(entry)); }

		[[nodiscard]] const std::vector<UndoEntry> &entries() const { return entries_; }

		void clear() { entries_.clear(); }

		[[nodiscard]] bool empty() const { return entries_.empty(); }

		[[nodiscard]] size_t size() const { return entries_.size(); }

	private:
		std::vector<UndoEntry> entries_;
	};

} // namespace graph::storage::wal
