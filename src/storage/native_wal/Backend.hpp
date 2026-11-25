#pragma once
#include <memory>
#include <string>
#include "graph/storage/StorageBackend.hpp"
namespace graph::storage {
	std::shared_ptr<StorageBackend> openNativeWalBackend(const std::string &path, bool readOnly, size_t memory);
}
