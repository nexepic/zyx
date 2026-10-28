#pragma once

#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::storage {
// Experimental synchronous presence probe. The owner must implement both PMR
// allocation and unmanaged reservation accounting and remain live until all
// allocations it owns are released. No borrowed value is returned. A route
// without a verified admission contract may throw LogicalGraphBufferPressure
// before producing a result so a caller can use its established fallback.
class LogicalGraphBudgetedNonNullProvider {
public:
    virtual ~LogicalGraphBudgetedNonNullProvider() = default;
    [[nodiscard]] virtual bool nodePropertyIsNonNullWithOwner(
        uint64_t id, std::string_view key,
        std::shared_ptr<std::pmr::memory_resource> owner) const = 0;
};
// Optional, storage-neutral synchronous capability. Implementations must use
// the same pinned snapshot for values and non-NULL probes. It does not grant
// authority beyond the invocation or permit borrowed spans/views to escape.
// IDs are strictly increasing, nonzero and bounded by MAXIMUM_ROWS. Presence
// is emitted exactly once per requested owner/key, with false for missing or
// NULL; the probe must not materialize large or complex value payloads merely
// to determine non-NULL. Value sinks retain their existing ordinal contract.
// Admission must occur before source allocation or delivery. A false return is
// permitted only before any sink invocation; after a sink has been invoked,
// exceptions propagate and the producer must never replay a delivered prefix.
// An implementation must fence mutation/epoch changes, authenticate competing
// ROW routes, and protect against callback reentry. Snapshots without this
// capability use the existing separate property/presence path.
struct JointNodePropertyReadSinks {
    LogicalGraphReadSnapshot::PropertyCellSink values;
    PropertyColumnBatchReadSinks runs;
    std::function<void(size_t, size_t, bool)> nonnull;
};
// A source may also be supplied by a scan visitor, but only while that
// visitor's authenticated source grant is synchronously active.
using JointNodePropertyReadSource = std::function<bool(
    std::span<const uint64_t>, std::span<const std::string_view>, std::span<const std::string_view>,
    const JointNodePropertyReadSinks &, const std::function<void(size_t)> &)>;
class LogicalGraphJointPropertyReadProvider {
public:
    static constexpr size_t MAXIMUM_ROWS = 512U;
    virtual ~LogicalGraphJointPropertyReadProvider() = default;
    [[nodiscard]] virtual bool tryReadJointNodePropertyWindow(
        std::span<const uint64_t> ids, std::span<const std::string_view> valueKeys,
        std::span<const std::string_view> nonnullKeys, const JointNodePropertyReadSinks &sinks,
        const std::function<void(size_t)> &admitSource) const = 0;
};
}
