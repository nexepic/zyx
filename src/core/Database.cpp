/**
 * @file Database
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Database.h"
#include <filesystem>

namespace graph {

Database::Database(const std::string& dbPath, size_t cacheSize)
    : dbPath(dbPath),
      openFlag(false) {
    storage = std::make_shared<storage::FileStorage>(dbPath, cacheSize);
}

Database::~Database() {
    close();
}

void Database::open() {
    if (openFlag) return;

    storage->open();
    queryEngine = std::make_shared<query::QueryEngine>(storage);
    queryEngine->initialize();
    openFlag = true;
}

void Database::close() {
    if (!openFlag) return;

    save();
    storage->close();
    queryEngine.reset();
    openFlag = false;
}

void Database::save() {
    if (openFlag) {
        storage->save();
    }
}

void Database::flush() {
    if (openFlag) {
        storage->flush();
    }
}

bool Database::exists() const {
    return std::filesystem::exists(dbPath);
}

Transaction Database::beginTransaction() {
    if (!openFlag) {
        open();
    }

    storage::FileStorage::beginWrite();
    return Transaction(*this);
}

Node Database::getNode(uint64_t id) {
    if (!openFlag) {
        open();
    }

    return storage->getNode(id);
}

Edge Database::getEdge(uint64_t id) {
    if (!openFlag) {
        open();
    }

    return storage->getEdge(id);
}

std::vector<Node> Database::getNodes(const std::vector<uint64_t>& ids) {
    if (!openFlag) {
        open();
    }

    return storage->getNodes(ids);
}

std::vector<Edge> Database::getEdges(const std::vector<uint64_t>& ids) {
    if (!openFlag) {
        open();
    }

    return storage->getEdges(ids);
}

std::vector<Node> Database::getNodesInRange(uint64_t startId, uint64_t endId, size_t limit) {
    if (!openFlag) {
        open();
    }

    return storage->getNodesInRange(startId, endId, limit);
}

std::vector<Edge> Database::getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit) {
    if (!openFlag) {
        open();
    }

    return storage->getEdgesInRange(startId, endId, limit);
}

} // namespace graph