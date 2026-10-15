#pragma once

#include <memory>
#include <memory_resource>
#include "graph/storage/StorageIO.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/OwnedPmrObject.hpp"

namespace graph::storage {
// Construct outside the publication gate. Its thread-bound scopes must be
// destroyed on the same worker, after all task-local buffers have been freed.
class RelocatableMaintenanceWorkspace final : public utils::OwnedPmrObject {
public:
    RelocatableMaintenanceWorkspace(std::shared_ptr<StorageIO> io,
            std::shared_ptr<std::pmr::memory_resource> memory)
        : io_(std::move(io)), allocation_(std::move(memory)), sequential_(*requireIo(io_)) {}
    RelocatableMaintenanceWorkspace(const RelocatableMaintenanceWorkspace &) = delete;
    RelocatableMaintenanceWorkspace &operator=(const RelocatableMaintenanceWorkspace &) = delete;
private:
    static StorageIO *requireIo(const std::shared_ptr<StorageIO> &io) {
        if (!io) throw std::invalid_argument("maintenance workspace requires storage IO");
        return io.get();
    }
    std::shared_ptr<StorageIO> io_;
    utils::OwnedAllocationScope allocation_;
    StorageIO::SequentialReadScope sequential_;
};
} // namespace graph::storage
