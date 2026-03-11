/**
 * @file WALApplier.hpp
 * @author Nexepic
 * @date 2025/3/24
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

#include "graph/storage/wal/WALRecord.hpp"
#include "graph/storage/FileStorage.hpp"
#include <functional>

namespace graph::storage::wal {

/**
 * @brief WAL Record Applier
 *
 * Bridge between WAL and FileStorage. Applies WAL records to the main database
 * file during checkpoint and crash recovery.
 *
 * This class handles:
 * - Node insert/update/delete operations
 * - Edge insert/update/delete operations
 * - Reference integrity validation
 * - Error handling and rollback
 */
class WALApplier {
public:
    /**
     * @brief Construct WAL applier
     * @param fileStorage File storage to apply records to
     */
    explicit WALApplier(std::shared_ptr<FileStorage> fileStorage);

    /**
     * @brief Apply a WAL record to storage
     * @param record WAL record to apply
     * @throws std::runtime_error if application fails
     */
    void applyRecord(const WALRecord* record);

    /**
     * @brief Create apply callback for WALManager
     * @return Function that applies WAL records
     *
     * This callback can be passed to WALManager::setApplyCallback()
     * to enable automatic record application during checkpoint.
     */
    [[nodiscard]] std::function<void(const WALRecord*)> createApplyCallback() const;

private:
    /**
     * @brief Apply node insert record
     * @param record Node record to apply
     */
    void applyNodeInsert(const WALRecord* record);

    /**
     * @brief Apply node update record
     * @param record Node record to apply
     */
    void applyNodeUpdate(const WALRecord* record);

    /**
     * @brief Apply node delete record
     * @param record Node record to apply
     */
    void applyNodeDelete(const WALRecord* record);

    /**
     * @brief Apply edge insert record
     * @param record Edge record to apply
     */
    void applyEdgeInsert(const WALRecord* record);

    /**
     * @brief Apply edge update record
     * @param record Edge record to apply
     */
    void applyEdgeUpdate(const WALRecord* record);

    /**
     * @brief Apply edge delete record
     * @param record Edge record to apply
     */
    void applyEdgeDelete(const WALRecord* record);

    /**
     * @brief Validate edge reference integrity
     * @param fromId Source node ID
     * @param toId Target node ID
     * @throws std::runtime_error if nodes don't exist
     */
    void validateEdgeReferences(int64_t fromId, int64_t toId) const;

    // File storage
    std::shared_ptr<FileStorage> fileStorage_;
};

} // namespace graph::storage::wal
