#pragma once

#include <memory>
#include <memory_resource>

namespace graph::storage {
// Optional allocation boundary for a transaction's private retained state.
// Overlays use the same WRITE owner for their nodes and statement undo metadata.
// Access is confined to the transaction's existing mutation thread contract.
class LogicalGraphTransactionMemoryProvider {
public:
    virtual ~LogicalGraphTransactionMemoryProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<std::pmr::memory_resource> retainedWriteMemory() const = 0;
};
} // namespace graph::storage
