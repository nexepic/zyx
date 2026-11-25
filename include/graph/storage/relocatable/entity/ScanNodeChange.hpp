#pragma once

#include <cstdint>
#include <span>

namespace graph::storage::scan_candidate_detail {
    enum class ScanNodeChangeKind : uint8_t { SNCK_CREATE = 1, SNCK_DELETE, SNCK_RELABEL };
    struct ScanNodeChange {
        uint64_t id = 0;
        ScanNodeChangeKind kind = ScanNodeChangeKind::SNCK_CREATE;
        std::span<const uint64_t> labels;
    };
}
