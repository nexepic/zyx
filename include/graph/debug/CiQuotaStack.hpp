#pragma once
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
namespace graph::debug {
// Installed only by the test executable before workers start. Production builds
// have neither the callback nor any stack-capture dependency.
inline void (*ciQuotaStackCapture)() noexcept = nullptr;
inline void captureCiQuotaStack() noexcept {
    if (ciQuotaStackCapture) ciQuotaStackCapture();
}
}
#endif
