#pragma once
#include <memory>
#include <string>
#include "graph/storage/StorageBackend.hpp"
namespace graph::storage {
	std::shared_ptr<StorageBackend> openSegmentedBackend(const std::string &path, size_t cacheBytes);
	std::shared_ptr<StorageBackend> openCheckpointBackend(const std::string &path, size_t cacheBytes);
}
