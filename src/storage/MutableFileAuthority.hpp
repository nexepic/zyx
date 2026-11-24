/**
 * @file MutableFileAuthority.hpp
 * @brief Process-local visibility of physical writes to shared native pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

namespace graph::storage {
    class StorageIO;
    namespace detail {
        // Process-local domain of the opened file identity. No lock holder
        // may acquire a buffer-pool lock: cache admission may itself flush.
        struct MutableFileAuthority {
            std::mutex mutex;
            uint64_t version = 1;
            // An opaque cache address identifies a contiguous run of writes
            // that preserved that cache's overlapping views. Every other
            // reader must observe the latest physical version.
            uintptr_t coherent_cache = 0;
            uint64_t before_coherent_writes = 1;
            uint64_t observedBy(const void *cache) const noexcept {
                return version != UINT64_MAX && coherent_cache == reinterpret_cast<uintptr_t>(cache)
                    ? before_coherent_writes : version;
            }
        };
        void bindMutableFile(StorageIO &io, std::shared_ptr<MutableFileAuthority> file, std::shared_ptr<void> coordination = {});
        std::shared_ptr<MutableFileAuthority> mutableFile(const StorageIO &io);
    }
}
