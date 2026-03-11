/**
 * @file WALApplier.cpp
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

#include "graph/storage/wal/WALApplier.hpp"
#include <stdexcept>
#include <iostream>

namespace graph::storage::wal {

// ============================================================================
// Constructor
// ============================================================================

WALApplier::WALApplier(std::shared_ptr<FileStorage> fileStorage)
    : fileStorage_(std::move(fileStorage)) {
    if (!fileStorage_) {
        throw std::runtime_error("FileStorage cannot be null");
    }
}

// ============================================================================
// Record Application
// ============================================================================

void WALApplier::applyRecord(const WALRecord* record) {
    if (!record) {
        throw std::runtime_error("Cannot apply null WAL record");
    }

    switch (record->getType()) {
        case WALRecordType::NODE_INSERT:
            applyNodeInsert(record);
            break;

        case WALRecordType::NODE_UPDATE:
            applyNodeUpdate(record);
            break;

        case WALRecordType::NODE_DELETE:
            applyNodeDelete(record);
            break;

        case WALRecordType::EDGE_INSERT:
            applyEdgeInsert(record);
            break;

        case WALRecordType::EDGE_UPDATE:
            applyEdgeUpdate(record);
            break;

        case WALRecordType::EDGE_DELETE:
            applyEdgeDelete(record);
            break;

        case WALRecordType::TRANSACTION_BEGIN:
        case WALRecordType::TRANSACTION_COMMIT:
        case WALRecordType::TRANSACTION_ROLLBACK:
        case WALRecordType::CHECKPOINT:
            // Transaction control records are not applied to storage
            break;

        default:
            throw std::runtime_error("Unknown WAL record type: " +
                                    std::to_string(static_cast<int>(record->getType())));
    }
}

std::function<void(const WALRecord*)> WALApplier::createApplyCallback() const {
    return [this](const WALRecord* record) {
        // Const_cast is safe here because we're creating a callback
        // that will be used by WALManager (non-const context)
        const_cast<WALApplier*>(this)->applyRecord(record);
    };
}

// ============================================================================
// Node Operations
// ============================================================================

void WALApplier::applyNodeInsert(const WALRecord* record) {
    auto* nodeRecord = dynamic_cast<const NodeRecord*>(record);
    if (!nodeRecord) {
        throw std::runtime_error("Invalid node record type");
    }

    const auto& node = nodeRecord->getNode();

    // TODO: Apply node insert to FileStorage
    // fileStorage_->insertNode(node);

    std::cout << "Applied node insert: ID=" << node.getId()
              << ", label=" << node.getLabel() << "\n";
}

void WALApplier::applyNodeUpdate(const WALRecord* record) {
    auto* nodeRecord = dynamic_cast<const NodeRecord*>(record);
    if (!nodeRecord) {
        throw std::runtime_error("Invalid node record type");
    }

    const auto& node = nodeRecord->getNode();

    // TODO: Apply node update to FileStorage
    // fileStorage_->updateNode(node);

    std::cout << "Applied node update: ID=" << node.getId()
              << ", label=" << node.getLabel() << "\n";
}

void WALApplier::applyNodeDelete(const WALRecord* record) {
    auto* nodeRecord = dynamic_cast<const NodeRecord*>(record);
    if (!nodeRecord) {
        throw std::runtime_error("Invalid node record type");
    }

    int64_t nodeId = nodeRecord->getNode().getId();

    // TODO: Apply node delete to FileStorage
    // fileStorage_->deleteNode(nodeId);

    std::cout << "Applied node delete: ID=" << nodeId << "\n";
}

// ============================================================================
// Edge Operations
// ============================================================================

void WALApplier::applyEdgeInsert(const WALRecord* record) {
    auto* edgeRecord = dynamic_cast<const EdgeRecord*>(record);
    if (!edgeRecord) {
        throw std::runtime_error("Invalid edge record type");
    }

    const auto& edge = edgeRecord->getEdge();

    // Validate references
    validateEdgeReferences(edge.getFromNodeId(), edge.getToNodeId());

    // TODO: Apply edge insert to FileStorage
    // fileStorage_->insertEdge(edge);

    std::cout << "Applied edge insert: ID=" << edge.getId()
              << ", from=" << edge.getFromNodeId()
              << ", to=" << edge.getToNodeId()
              << ", label=" << edge.getLabel() << "\n";
}

void WALApplier::applyEdgeUpdate(const WALRecord* record) {
    auto* edgeRecord = dynamic_cast<const EdgeRecord*>(record);
    if (!edgeRecord) {
        throw std::runtime_error("Invalid edge record type");
    }

    const auto& edge = edgeRecord->getEdge();

    // Validate references
    validateEdgeReferences(edge.getFromNodeId(), edge.getToNodeId());

    // TODO: Apply edge update to FileStorage
    // fileStorage_->updateEdge(edge);

    std::cout << "Applied edge update: ID=" << edge.getId()
              << ", from=" << edge.getFromNodeId()
              << ", to=" << edge.getToNodeId()
              << ", label=" << edge.getLabel() << "\n";
}

void WALApplier::applyEdgeDelete(const WALRecord* record) {
    auto* edgeRecord = dynamic_cast<const EdgeRecord*>(record);
    if (!edgeRecord) {
        throw std::runtime_error("Invalid edge record type");
    }

    int64_t edgeId = edgeRecord->getEdge().getId();

    // TODO: Apply edge delete to FileStorage
    // fileStorage_->deleteEdge(edgeId);

    std::cout << "Applied edge delete: ID=" << edgeId << "\n";
}

// ============================================================================
// Reference Validation
// ============================================================================

void WALApplier::validateEdgeReferences(int64_t fromId, int64_t toId) const {
    // TODO: Validate that both nodes exist
    // This is critical for graph database integrity

    // auto fromNode = fileStorage_->getNode(fromId);
    // if (!fromNode.has_value()) {
    //     throw std::runtime_error("Source node does not exist: " + std::to_string(fromId));
    // }

    // auto toNode = fileStorage_->getNode(toId);
    // if (!toNode.has_value()) {
    //     throw std::runtime_error("Target node does not exist: " + std::to_string(toId));
    // }

    (void)fromId;  // Suppress unused warning
    (void)toId;    // Suppress unused warning
}

} // namespace graph::storage::wal
