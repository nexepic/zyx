#pragma once
#include "graph/storage/PreadHelper.hpp"
#ifdef _WIN32
#include "graph/storage/detail/WindowsLeaseReadBridge.hpp"
#endif
namespace graph::storage::testing {
// Preserve StorageIO's lease semantics in pass-through fault-hook reads.
inline pwrite_ssize_t readWithNativeLease(file_handle_t fd, void *buffer, size_t bytes, pwrite_off_t offset) {
    auto result = static_cast<pwrite_ssize_t>(portable_pread(fd, buffer, bytes, offset));
#ifdef _WIN32
    if (result < 0 && offset == 0 && ::GetLastError() == ERROR_LOCK_VIOLATION) {
        const auto borrowed = detail::WindowsLeaseReadBridge::readLockedByte(fd, buffer, bytes);
        if (borrowed) { ::SetLastError(borrowed->error); return borrowed->bytes; }
        result = static_cast<pwrite_ssize_t>(portable_pread(fd, buffer, bytes, offset));
    }
#endif
    return result;
}
}
