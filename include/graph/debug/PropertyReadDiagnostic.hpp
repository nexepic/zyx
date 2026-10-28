#pragma once
#if defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
#include <cstdint>
#include <limits>
#include "graph/debug/PerfTrace.hpp"
namespace graph::debug {
enum class PropertyDiagnosticStage { PDS_OTHER, PDS_SELECTED, PDS_NONNULL };
inline thread_local PropertyDiagnosticStage property_diagnostic_stage = PropertyDiagnosticStage::PDS_OTHER;
class ScopedPropertyReadDiagnostic final {
public:
    explicit ScopedPropertyReadDiagnostic(PropertyDiagnosticStage stage) : previous_(property_diagnostic_stage) { property_diagnostic_stage = stage; }
    ~ScopedPropertyReadDiagnostic() { property_diagnostic_stage = previous_; }
private: PropertyDiagnosticStage previous_;
};
inline void recordPropertyOwnerAuthentication(uint64_t id) {
    if (id > uint64_t((std::numeric_limits<int64_t>::max)())) return;
    const char *key = "test.owner_auth.other";
    if (property_diagnostic_stage == PropertyDiagnosticStage::PDS_SELECTED) key = "test.owner_auth.selected";
    else if (property_diagnostic_stage == PropertyDiagnosticStage::PDS_NONNULL) key = "test.owner_auth.nonnull";
    PerfTrace::addValue(key, int64_t(id));
}
}
#endif
