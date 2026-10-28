#pragma once
#ifdef _WIN32

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <stdexcept>
#include "graph/storage/PreadHelper.hpp"

namespace graph::storage::detail {
// One process-local borrow registry for the existing byte-zero native lease.
// Callers only receive bytes, never the write-capable handle. Holding mutex_
// across a borrow pins the lease handle until unlock and deregistration.
class WindowsLeaseReadBridge final {
    using Identity = std::array<DWORD, 3>;
    struct Registry {
        std::mutex mutex;
        std::map<Identity, file_handle_t> owners;
    };
    static Registry &registry() { static Registry *value = new Registry; return *value; }
    static std::optional<Identity> identity(file_handle_t handle) noexcept {
        BY_HANDLE_FILE_INFORMATION information = {};
        if (!::GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle), &information)) return std::nullopt;
        return Identity{information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow};
    }
public:
    // The lease owns a non-inheritable duplicate and locks that exact handle.
    // Its value cannot be reused while the registry contains it, even if the
    // backend's original writer handle closes before an outstanding borrow.
    static file_handle_t tryLock(file_handle_t handle) {
        auto &state = registry();
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (!::DuplicateHandle(::GetCurrentProcess(), reinterpret_cast<HANDLE>(handle), ::GetCurrentProcess(),
                               &duplicate, 0U, FALSE, DUPLICATE_SAME_ACCESS))
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "relocatable bulk session could not retain the database handle");
        struct HandleGuard {
            HANDLE handle;
            ~HandleGuard() { if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle); }
        } retained{duplicate};
        const auto id = identity(reinterpret_cast<file_handle_t>(duplicate));
        if (!id) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                         "relocatable bulk session could not identify the database");
        std::lock_guard guard(state.mutex);
        OVERLAPPED overlapped = {};
        if (!::LockFileEx(duplicate, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                          0U, 1U, 0U, &overlapped)) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_LOCK_VIOLATION) return INVALID_FILE_HANDLE;
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "relocatable bulk session could not lock the database");
        }
        const auto token = reinterpret_cast<file_handle_t>(duplicate);
        try {
            if (!state.owners.emplace(*id, token).second)
                throw std::logic_error("duplicate process-local bulk session lease");
        } catch (...) {
            static_cast<void>(::UnlockFileEx(duplicate, 0U, 1U, 0U, &overlapped));
            throw;
        }
        retained.handle = INVALID_HANDLE_VALUE;
        return token;
    }
    static void unlock(file_handle_t token) noexcept {
        auto &state = registry();
        std::lock_guard guard(state.mutex);
        bool found = false;
        for (auto it = state.owners.begin(); it != state.owners.end(); ++it) {
            if (it->second == token) { state.owners.erase(it); found = true; break; }
        }
        if (!found) return;
        OVERLAPPED overlapped = {};
        const HANDLE handle = reinterpret_cast<HANDLE>(token);
        static_cast<void>(::UnlockFileEx(handle, 0U, 1U, 0U, &overlapped));
        ::CloseHandle(handle);
    }
    static std::optional<pwrite_ssize_t> readLockedByte(file_handle_t reader, void *buffer, size_t bytes) {
        const auto id = identity(reader);
        if (!id) return std::nullopt;
        auto &state = registry();
        std::lock_guard guard(state.mutex);
        const auto owner = state.owners.find(*id);
        if (owner == state.owners.end()) return std::nullopt;
        return static_cast<pwrite_ssize_t>(portable_pread(owner->second, buffer, bytes, 0));
    }
};
}
#endif
