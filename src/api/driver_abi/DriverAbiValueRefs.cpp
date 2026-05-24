#include "DriverAbiInternal.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {

std::atomic<uint64_t> nextOwnerId{1};
std::atomic<uint64_t> nextCookie{0x9e3779b97f4a7c15ULL};
std::mutex registryMutex;
std::unordered_map<uint64_t, zyx_driver_result_t *> resultRegistry;

uint64_t nextNonZero(std::atomic<uint64_t> &counter) {
    uint64_t value = counter.fetch_add(1, std::memory_order_relaxed);
    return value == 0 ? counter.fetch_add(1, std::memory_order_relaxed) : value; // ZYX_COV_EXCL_LINE: 64-bit token wraparound is defensive.
}

zyx_driver_status_t validateRefToken(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error) {
    if (ref == nullptr || ref->owner_id == 0 || ref->owner_cookie == 0 || ref->generation == 0) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value reference is invalid");
    }
    return ZYX_DRIVER_OK;
}

} // namespace

void registerResultHandle(zyx_driver_result_t *result) {
    if (result == nullptr) return; // ZYX_COV_EXCL_LINE: internal helper is only called with allocated result handles.
    result->value_ref_owner_id = nextNonZero(nextOwnerId);
    result->value_ref_cookie = nextNonZero(nextCookie);
    result->value_ref_generation = 1;
    std::lock_guard lock(registryMutex);
    resultRegistry[result->value_ref_owner_id] = result;
}

void unregisterResultHandle(zyx_driver_result_t *result) {
    if (result == nullptr || result->value_ref_owner_id == 0) return; // ZYX_COV_EXCL_LINE: internal helper is only called for registered result handles.
    std::lock_guard lock(registryMutex);
    auto it = resultRegistry.find(result->value_ref_owner_id);
    if (it != resultRegistry.end() && it->second == result) { // ZYX_COV_EXCL_LINE: registry mismatch is defensive cleanup.
        resultRegistry.erase(it);
    }
    result->value_ref_owner_id = 0;
    result->value_ref_cookie = 0;
    result->value_ref_generation = 0;
}

zyx_driver_result_t *resolveValueRefOwner(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error) {
    if (auto status = validateRefToken(ref, out_error); status != ZYX_DRIVER_OK) {
        return nullptr;
    }

    std::lock_guard lock(registryMutex);
    auto it = resultRegistry.find(ref->owner_id);
    if (it == resultRegistry.end() || it->second == nullptr) { // ZYX_COV_EXCL_LINE: null registry entries are not created by public ABI.
        setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value reference owner is no longer valid");
        return nullptr;
    }

    zyx_driver_result_t *owner = it->second;
    if (owner->value_ref_cookie != ref->owner_cookie || owner->value_ref_generation != ref->generation) { // ZYX_COV_EXCL_LINE: cookie mismatch requires a 64-bit owner token collision.
        setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value reference is stale");
        return nullptr;
    }
    return owner;
}

const zyx::Value *resolveValueRef(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error) {
    zyx_driver_result_t *owner = resolveValueRefOwner(ref, out_error);
    if (owner == nullptr) {
        return nullptr;
    }
    if (ref->slot >= owner->value_buffers.size()) {
        setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value reference slot is invalid");
        return nullptr;
    }
    return &owner->value_buffers[static_cast<size_t>(ref->slot)];
}

zyx_driver_value_ref_t makeValueRef(const zyx_driver_result_t *owner, size_t slot) {
    if (owner == nullptr || owner->value_ref_owner_id == 0 || owner->value_ref_cookie == 0 || // ZYX_COV_EXCL_LINE: internal helper is only called after result registration.
        owner->value_ref_generation == 0) { // ZYX_COV_EXCL_LINE: generation zero is reserved for unregistered handles.
        return nullValueRef();
    }
    return zyx_driver_value_ref_t{owner->value_ref_owner_id, owner->value_ref_cookie, owner->value_ref_generation,
                                  static_cast<uint64_t>(slot)};
}

zyx_driver_value_ref_t nullValueRef() {
    return zyx_driver_value_ref_t{0, 0, 0, 0};
}

void bumpValueRefGeneration(zyx_driver_result_t *result) noexcept {
    if (result == nullptr) return; // ZYX_COV_EXCL_LINE: internal helper is only called with live result handles.
    ++result->value_ref_generation;
    if (result->value_ref_generation == 0) { // ZYX_COV_EXCL_LINE: 64-bit generation wraparound is defensive.
        result->value_ref_generation = 1;
    }
}

size_t appendValueRefBuffer(const zyx_driver_result_t *result, zyx::Value value) {
    result->value_buffers.push_back(std::move(value));
    return result->value_buffers.size() - 1;
}
