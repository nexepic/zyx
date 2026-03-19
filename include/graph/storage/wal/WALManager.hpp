/**
 * @file WALManager.hpp
 * @date 2026/3/19
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

#include <fstream>
#include <string>
#include <vector>
#include "WALRecord.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph::storage::wal {

	class WALManager {
	public:
		WALManager() = default;
		~WALManager();

		void open(const std::string &dbPath);
		void close();

		void writeBegin(uint64_t txnId);
		void writeEntityChange(uint64_t txnId, uint8_t entityType, uint8_t changeType, int64_t entityId,
							   const std::vector<uint8_t> &serializedData);
		void writeCommit(uint64_t txnId);
		void writeRollback(uint64_t txnId);
		void sync();
		void checkpoint();

		[[nodiscard]] bool needsRecovery();
		[[nodiscard]] bool isOpen() const { return isOpen_; }
		[[nodiscard]] std::string getWALPath() const { return walPath_; }

	private:
		std::string walPath_;
		std::fstream walFile_;
		bool isOpen_ = false;

		void writeRecord(WALRecordType type, uint64_t txnId, const uint8_t *data = nullptr, uint32_t dataSize = 0);
		void writeHeader();
		[[nodiscard]] bool validateHeader();
	};

} // namespace graph::storage::wal
