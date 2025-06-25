/**
 * @file WALRecord.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/wal/WALRecordImpl.hpp"
#include <stdexcept>

namespace graph::storage::wal {

std::unique_ptr<WALRecord> WALRecord::createFromStream(std::istream& in) {
    if (!in) {
        throw std::runtime_error("Invalid input stream for WAL record");
    }

    WALRecordType type;
    in.read(reinterpret_cast<char*>(&type), sizeof(type));
    if (!in) {
        throw std::runtime_error("Failed to read WAL record type");
    }

    std::unique_ptr<WALRecord> record;
    switch (type) {
        case WALRecordType::NODE_INSERT:
        case WALRecordType::NODE_UPDATE:
        case WALRecordType::NODE_DELETE:
            record = std::make_unique<NodeRecord>(type);
            break;

        case WALRecordType::EDGE_INSERT:
        case WALRecordType::EDGE_UPDATE:
        case WALRecordType::EDGE_DELETE:
            record = std::make_unique<EdgeRecord>(type);
            break;

        case WALRecordType::CHECKPOINT:
            record = std::make_unique<CheckpointRecord>();
            break;

        case WALRecordType::TRANSACTION_BEGIN:
        case WALRecordType::TRANSACTION_COMMIT:
        case WALRecordType::TRANSACTION_ROLLBACK:
            record = std::make_unique<TransactionRecord>(type);
            break;

        default:
            throw std::runtime_error("Unknown WAL record type");
    }

    record->deserialize(in);
    return record;
}

void NodeRecord::serialize(uint8_t* buffer) const {
    // Serialize logic for NodeRecord
}

void NodeRecord::deserialize(std::istream& in) {
    // Deserialize logic for NodeRecord
}

size_t NodeRecord::getSize() const {
    return sizeof(WALRecordType) + node_.getSerializedSize();
}

void EdgeRecord::serialize(uint8_t* buffer) const {
    // Serialize logic for EdgeRecord
}

void EdgeRecord::deserialize(std::istream& in) {
    // Deserialize logic for EdgeRecord
}

size_t EdgeRecord::getSize() const {
    return sizeof(WALRecordType) + edge_.getSerializedSize();
}

void TransactionRecord::serialize(uint8_t* buffer) const {
    // Serialize logic for TransactionRecord
}

void TransactionRecord::deserialize(std::istream& in) {
    // Deserialize logic for TransactionRecord
}

size_t TransactionRecord::getSize() const {
    return sizeof(WALRecordType) + sizeof(uint64_t);
}

void CheckpointRecord::serialize(uint8_t* buffer) const {
    // Serialize logic for CheckpointRecord
}

void CheckpointRecord::deserialize(std::istream& in) {
    // Deserialize logic for CheckpointRecord
}

size_t CheckpointRecord::getSize() const {
    return sizeof(WALRecordType) + sizeof(uint64_t);
}

} // namespace graph::storage::wal