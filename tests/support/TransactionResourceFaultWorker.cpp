#include "graph/core/GuardedPropertyCopy.hpp"
#include "graph/storage/relocatable/entity/NativeReadSession.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/utils/OwnedStdString.hpp"
#include "graph/core/OwnedPropertyValue.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/utils/OwnedStringVector.hpp"
#include "graph/utils/OwnedStringSet.hpp"
/**
 * @file TransactionResourceFaultWorker.cpp
 * @brief Isolated allocation-failure and terminal transaction retention checks.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/LogicalQueryScratch.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/LogicalGraphTransactionMemoryProvider.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "storage/relocatable/backend/AdmittedMutationInput.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>

namespace {
std::atomic<bool> denyAllocations{false};
std::atomic<size_t> liveBytes{0U};
std::atomic<size_t> allocationAttempts{0U};
std::atomic<size_t> faultRemaining{(std::numeric_limits<size_t>::max)()};
std::atomic<bool> faultTriggered{false};
std::atomic<bool> captureBodyAllocation{false};
std::atomic<size_t> bodyAllocationRequest{0U}, bodyAllocationMatches{0U}, prototypeBodyRequest{0U};
thread_local graph::query::execution::MaterializedByteBudget *observedWriteBudget = nullptr;
thread_local graph::query::execution::detail::PrepaidMemoryResource *observedPrepaidBudget = nullptr;
thread_local size_t observedInitialLive = 0U, observedInitialCharge = 0U, maximumUncoveredWriteBytes = 0U;
void observeWriteOwnership(size_t live) noexcept {
    if (!observedWriteBudget && !observedPrepaidBudget) return;
    const auto allocated = static_cast<std::ptrdiff_t>(live) - static_cast<std::ptrdiff_t>(observedInitialLive);
    const auto charged = static_cast<std::ptrdiff_t>(observedWriteBudget ? observedWriteBudget->used() : observedPrepaidBudget->used()) - static_cast<std::ptrdiff_t>(observedInitialCharge);
    if (allocated > charged) maximumUncoveredWriteBytes = (std::max)(maximumUncoveredWriteBytes,
        static_cast<size_t>(allocated - charged));
}
struct alignas(std::max_align_t) AllocationHeader { void *base; size_t bytes; };

void *allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
    allocationAttempts.fetch_add(1U, std::memory_order_relaxed);
    if (captureBodyAllocation.load(std::memory_order_relaxed)) {
        const auto target = bodyAllocationRequest.load(std::memory_order_relaxed);
        if (target != 0U && bytes == target) bodyAllocationMatches.fetch_add(1U, std::memory_order_relaxed);
        if (target == 0U && bytes >= 65536U && bytes < 67584U) prototypeBodyRequest.store(bytes, std::memory_order_relaxed);
    }
    if (denyAllocations.load(std::memory_order_relaxed)) throw std::bad_alloc();
    size_t remaining = faultRemaining.load(std::memory_order_relaxed);
    while (remaining != (std::numeric_limits<size_t>::max)()) {
        if (remaining == 0U) {
            if (faultRemaining.compare_exchange_weak(remaining, (std::numeric_limits<size_t>::max)(), std::memory_order_relaxed)) {
                faultTriggered.store(true, std::memory_order_relaxed);
                throw std::bad_alloc();
            }
        } else if (faultRemaining.compare_exchange_weak(remaining, remaining - 1U, std::memory_order_relaxed)) break;
    }
    bytes = std::max(size_t{1U}, bytes);
    alignment = std::max(alignment, alignof(AllocationHeader));
    if (bytes > (std::numeric_limits<size_t>::max)() - sizeof(AllocationHeader) - alignment)
        throw std::bad_alloc();
    const size_t total = bytes + sizeof(AllocationHeader) + alignment;
    void *base = std::malloc(total);
    if (!base) throw std::bad_alloc();
    void *result = static_cast<std::byte *>(base) + sizeof(AllocationHeader);
    size_t available = total - sizeof(AllocationHeader);
    if (!std::align(alignment, bytes, result, available)) { std::free(base); throw std::bad_alloc(); }
    auto *header = static_cast<AllocationHeader *>(result) - 1;
    std::construct_at(header, AllocationHeader{base, bytes});
    observeWriteOwnership(liveBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes);
    return result;
}
void deallocate(void *pointer) noexcept {
    if (!pointer) return;
    auto *header = static_cast<AllocationHeader *>(pointer) - 1;
    observeWriteOwnership(liveBytes.fetch_sub(header->bytes, std::memory_order_relaxed) - header->bytes);
    std::free(header->base);
}
struct RejectAllocations {
    RejectAllocations() { denyAllocations.store(true, std::memory_order_relaxed); }
    ~RejectAllocations() { denyAllocations.store(false, std::memory_order_relaxed); }
};
} // namespace

// Only this independently selected executable replaces allocation. The normal
// test suite, production binaries and benchmark binaries retain their allocators.
void *operator new(size_t bytes) { return allocate(bytes); }
void *operator new[](size_t bytes) { return allocate(bytes); }
void operator delete(void *pointer) noexcept { deallocate(pointer); }
void operator delete[](void *pointer) noexcept { deallocate(pointer); }
void operator delete(void *pointer, size_t) noexcept { deallocate(pointer); }
void operator delete[](void *pointer, size_t) noexcept { deallocate(pointer); }
void *operator new(size_t bytes, std::align_val_t alignment) { return allocate(bytes, static_cast<size_t>(alignment)); }
void *operator new[](size_t bytes, std::align_val_t alignment) { return allocate(bytes, static_cast<size_t>(alignment)); }
void operator delete(void *pointer, std::align_val_t) noexcept { deallocate(pointer); }
void operator delete[](void *pointer, std::align_val_t) noexcept { deallocate(pointer); }
void operator delete(void *pointer, size_t, std::align_val_t) noexcept { deallocate(pointer); }
void operator delete[](void *pointer, size_t, std::align_val_t) noexcept { deallocate(pointer); }
void *operator new(size_t bytes, const std::nothrow_t &) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void *operator new[](size_t bytes, const std::nothrow_t &) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void operator delete(void *pointer, const std::nothrow_t &) noexcept { deallocate(pointer); }
void operator delete[](void *pointer, const std::nothrow_t &) noexcept { deallocate(pointer); }
void *operator new(size_t bytes, std::align_val_t alignment, const std::nothrow_t &) noexcept {
    try { return allocate(bytes, static_cast<size_t>(alignment)); } catch (...) { return nullptr; }
}
void *operator new[](size_t bytes, std::align_val_t alignment, const std::nothrow_t &) noexcept {
    try { return allocate(bytes, static_cast<size_t>(alignment)); } catch (...) { return nullptr; }
}
void operator delete(void *pointer, std::align_val_t, const std::nothrow_t &) noexcept { deallocate(pointer); }
void operator delete[](void *pointer, std::align_val_t, const std::nothrow_t &) noexcept { deallocate(pointer); }

using namespace graph;
using namespace graph::storage;
namespace {
uint64_t stageNodes(LogicalGraphTransaction &transaction, size_t count, size_t payloadBytes = 0U) {
    LogicalGraphMutationBatch batch;
    batch.node_creates.resize(count);
    if (payloadBytes) batch.node_creates.front().properties.push_back({"payload", PropertyValue(std::string(payloadBytes, 'q'))});
    const auto result = transaction.stageMutationBatch(std::move(batch));
    if (result.created_node_ids.size() != count) throw std::runtime_error("staged identity count changed");
    return result.created_node_ids.back();
}
size_t destroyClosed(std::unique_ptr<LogicalGraphTransaction> &transaction) {
    const size_t before = liveBytes.load(std::memory_order_relaxed);
    transaction.reset();
    const size_t after = liveBytes.load(std::memory_order_relaxed);
    if (after > before) throw std::runtime_error("closed transaction destruction allocated memory");
    const size_t retained = before - after;
    // The closed object itself is expected; payloads, ID arrays and their spare
    // capacity must already have been released. This is not an RSS measurement.
    if (retained >= 16U * 1024U) throw std::runtime_error("closed transaction retained payload or ID capacity");
    return retained;
}

LogicalGraphMutationBatch propertyReplacement(uint64_t id, int64_t value) {
    LogicalGraphMutationBatch batch;
    batch.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id},
        {{"value", PropertyValue(value)}, {"payload", PropertyValue(std::string(4096U, 'p'))}}}};
    return batch;
}
void allocationFaultCase(const std::string &mode, RelocatableStorageBackend &backend, size_t fault) {
    const uint64_t id = backend.createLogicalNodes({{{}, {{"value", PropertyValue(int64_t{0})}}}}).front();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    const bool useOverlay = mode == "overlay-stage-allocation-fault";
    const bool stage = mode == "stage-allocation-fault" || useOverlay;
    std::unique_ptr<LogicalGraphTransactionOverlay> overlay;
    if (useOverlay) {
        overlay = std::make_unique<LogicalGraphTransactionOverlay>(*transaction);
        (void)overlay->executeLogicalGraphMutationBatch(propertyReplacement(id, 1));
    } else {
        (void)transaction->stageMutationBatch(propertyReplacement(id, 1));
    }
    auto candidate = stage ? propertyReplacement(id, 2) : LogicalGraphMutationBatch{};
    bool threw = false;
    faultTriggered.store(false, std::memory_order_relaxed);
    const size_t before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    try {
        if (useOverlay) (void)overlay->executeLogicalGraphMutationBatch(candidate);
        else if (stage) (void)transaction->stageMutationBatch(std::move(candidate));
        else (void)transaction->commit();
    } catch (const std::exception &) { threw = true; }
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const size_t attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    const bool triggered = faultTriggered.load(std::memory_order_relaxed);
    const auto state = transaction->state();
    if (!triggered && threw) throw std::runtime_error("normal allocation probe failed");
    if (stage) {
        if (state != LogicalGraphTransactionState::LGTTS_ACTIVE)
            throw std::runtime_error("Property staging allocation fault finished its transaction");
        if (useOverlay) {
            auto visible = overlay->openLogicalGraphReadSnapshot();
            if (visible->readNodeProperty(id, "value") != PropertyValue(int64_t{threw ? 1 : 2}) ||
                visible->readNodeProperty(id, "payload") != PropertyValue(std::string(4096U, 'p')))
                throw std::runtime_error("failed overlay allocation changed the visible statement prefix");
        }
        (void)transaction->commit();
    } else if (state == LogicalGraphTransactionState::LGTTS_ACTIVE) {
        throw std::runtime_error("commit allocation fault left a reusable transaction");
    }
    overlay.reset();
    const size_t retained = destroyClosed(transaction);
    backend.close(); backend.open();
    int64_t value = -1;
    {
        auto snapshot = backend.openLogicalGraphReadSnapshot();
        const auto observed = snapshot->readNodeProperty(id, "value");
        for (int64_t expected = 0; expected <= 2; ++expected)
            if (observed == PropertyValue(expected)) value = expected;
        const auto payload = snapshot->readNodeProperty(id, "payload");
        if (value == 0 ? payload != PropertyValue() : payload != PropertyValue(std::string(4096U, 'p')))
            throw std::runtime_error("allocation fault published an incomplete Property replacement");
    }
    if (stage) {
        if (value != (threw ? 1 : 2)) throw std::runtime_error("failed Property statement changed its prior staged prefix");
    } else {
        if (value != 0 && value != 1) throw std::runtime_error("commit allocation fault published a partial Property value");
        if (!threw && value != 1) throw std::runtime_error("successful commit lost its Property replacement");
        if (threw && state != LogicalGraphTransactionState::LGTTS_INDETERMINATE && value != 0)
            throw std::runtime_error("definite failed commit nevertheless published its Property replacement");
    }
    (void)backend.verifyIntegrity(); backend.close();
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"fault_triggered\":" << (triggered ? "true" : "false")
        << ",\"operation_threw\":" << (threw ? "true" : "false")
        << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null";
    else std::cout << fault;
    std::cout << ",\"observed_value\":" << value << ",\"closed_object_bytes\":" << retained << ",\"passed\":true}\n";
}

void creationAllocationFaultCase(const std::string &mode, RelocatableStorageBackend &backend, size_t fault) {
    const auto creation = [](int64_t value) {
        LogicalGraphMutationBatch batch;
        batch.node_creates = {{{}, {{"value", PropertyValue(value)},
            {"payload", PropertyValue(std::string(4096U, 'c'))}}}};
        return batch;
    };
    auto transaction = backend.beginLogicalGraphTransaction(false);
    const bool useOverlay = mode == "overlay-creation-stage-allocation-fault";
    auto overlay = useOverlay ? std::make_unique<LogicalGraphTransactionOverlay>(*transaction) : nullptr;
    const auto first = useOverlay ? overlay->executeLogicalGraphMutationBatch(creation(1))
                                  : transaction->stageMutationBatch(creation(1));
    if (first.created_node_ids.size() != 1U) throw std::runtime_error("creation fault seed count changed");
    const auto firstId = first.created_node_ids.front();
    auto candidate = creation(2);
    const auto preview = transaction->previewMutationBatch(candidate);
    if (preview.created_node_ids.size() != 1U) throw std::runtime_error("creation fault preview count changed");
    const auto candidateId = preview.created_node_ids.front();
    bool threw = false;
    faultTriggered.store(false, std::memory_order_relaxed);
    const auto before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    try {
        if (useOverlay) (void)overlay->executeLogicalGraphMutationBatch(candidate);
        else (void)transaction->stageMutationBatch(std::move(candidate));
    }
    catch (const std::exception &) { threw = true; }
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const auto attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    const bool triggered = faultTriggered.load(std::memory_order_relaxed);
    if (!triggered && threw) throw std::runtime_error("normal creation allocation probe failed");
    if (transaction->state() != LogicalGraphTransactionState::LGTTS_ACTIVE)
        throw std::runtime_error("creation statement allocation fault finished its transaction");
    if (useOverlay) {
        auto visible = overlay->openLogicalGraphReadSnapshot();
        if (!visible->containsNode(firstId) || visible->containsNode(candidateId) == threw)
            throw std::runtime_error("creation allocation fault changed the visible overlay prefix");
    }
    (void)transaction->commit();
    overlay.reset();
    const size_t retained = destroyClosed(transaction);
    backend.close(); backend.open();
    {
        auto snapshot = backend.openLogicalGraphReadSnapshot();
        if (!snapshot->containsNode(firstId) || snapshot->readNodeProperty(firstId, "value") != PropertyValue(int64_t{1}) ||
            snapshot->readNodeProperty(firstId, "payload") != PropertyValue(std::string(4096U, 'c')))
            throw std::runtime_error("creation allocation fault changed its earlier statement");
        if (snapshot->containsNode(candidateId) == threw)
            throw std::runtime_error("failed creation receipt nevertheless changed the staged prefix");
        if (!threw && (snapshot->readNodeProperty(candidateId, "value") != PropertyValue(int64_t{2}) ||
            snapshot->readNodeProperty(candidateId, "payload") != PropertyValue(std::string(4096U, 'c'))))
            throw std::runtime_error("creation allocation fault published incomplete data");
    }
    (void)backend.verifyIntegrity(); backend.close();
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"fault_triggered\":" << (triggered ? "true" : "false")
        << ",\"operation_threw\":" << (threw ? "true" : "false")
        << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null";
    else std::cout << fault;
    std::cout << ",\"closed_object_bytes\":" << retained << ",\"passed\":true}\n";
}

void normalizedPatchAllocationFaultCase(const std::string &mode, RelocatableStorageBackend &backend, size_t fault) {
    const auto existing = backend.createLogicalNodes({{{"Original"}, {{"payload", PropertyValue(std::string(8192U, 'o'))}}}}).front();
    const auto fence = backend.currentNativeLogicalGraphMutationFence();
    const auto domain = backend.queryResourceDomain();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    auto overlay = std::make_unique<LogicalGraphTransactionOverlay>(*transaction);
    LogicalGraphMutationBatch prefix;
    prefix.node_creates = {{{"Prefix"}, {{"rank", PropertyValue(1)}}}};
    const auto prefixId = overlay->executeLogicalGraphMutationBatch(prefix).created_node_ids.at(0U);
    // Force the next statement to read an existing retained wide collection,
    // exercising private copies that do not enter the storage value decoder.
    overlay->replaceLogicalNodeProperties({{existing, {{"payload", PropertyValue(std::string(8192U, 'o'))}}}});
    auto source = overlay->openLogicalGraphReadSnapshot();
    LogicalGraphMutationBatch companion;
    companion.node_creates = {{{"Candidate"}, {{"payload", PropertyValue(std::string(16384U, 'c'))}}}};
    companion.node_label_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, existing}, {"Changed"}}};
    const std::vector<LogicalGraphPropertyPatchRequest> patches{
        {{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, existing},
            {{"payload", PropertyValue(std::string(65536U, 'n'))}}, {}},
        {{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
            {{"rank", PropertyValue(7)}}, {}}};
    const auto candidateId = transaction->previewMutationBatch(companion).created_node_ids.at(0U);
    LogicalGraphMutationReadControl control;
    control.property_admission = [](size_t bytes) {
        if (bytes > 1048576U) throw std::length_error("normalized source logical work exceeded its bound");
    };
    bool threw = false;
    faultTriggered.store(false, std::memory_order_relaxed);
    const auto before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    try { (void)overlay->executeLogicalGraphMutationBatchWithPropertyPatches(
        *source, companion, patches, 32U, *source->mutationFence(), control); }
    catch (const std::bad_alloc &) { threw = true; }
    catch (...) { faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed); throw; }
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const auto attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    const bool triggered = faultTriggered.load(std::memory_order_relaxed);
    if (!triggered && threw) throw std::runtime_error("normal normalized patch probe failed");
    if (transaction->state() != LogicalGraphTransactionState::LGTTS_ACTIVE ||
        backend.hasIndeterminateWriteOutcome() || backend.currentNativeLogicalGraphMutationFence() != fence)
        throw std::runtime_error("normalized patch fault changed transaction state or publication");
    {
        auto visible = overlay->openLogicalGraphReadSnapshot();
        const auto node = visible->readNodeShaped(existing, {.include_labels = true, .all_properties = true});
        const std::vector<std::string> expectedLabels{threw ? "Original" : "Changed"};
        if (!visible->containsNode(prefixId) || node.labels != expectedLabels ||
            visible->readNodeProperty(existing, "payload") != PropertyValue(std::string(threw ? 8192U : 65536U, threw ? 'o' : 'n')) ||
            visible->containsNode(candidateId) == threw)
            throw std::runtime_error("normalized patch fault changed the earlier overlay prefix");
    }
    if (threw) (void)overlay->executeLogicalGraphMutationBatchWithPropertyPatches(
        *source, companion, patches, 32U, *source->mutationFence(), control);
    source.reset();
    (void)transaction->commit(); overlay.reset();
    const auto retained = destroyClosed(transaction);
    backend.close(); backend.open();
    {
        auto snapshot = backend.openLogicalGraphReadSnapshot();
        if (!snapshot->containsNode(prefixId) || snapshot->readNodeProperty(prefixId, "rank") != PropertyValue(1) ||
            snapshot->readNodeProperty(candidateId, "rank") != PropertyValue(7) ||
            snapshot->readNodeProperty(candidateId, "payload") != PropertyValue(std::string(16384U, 'c')) ||
            snapshot->readNodeProperty(existing, "payload") != PropertyValue(std::string(65536U, 'n')))
            throw std::runtime_error("normalized patch retry lost its committed final state");
    }
    (void)backend.verifyIntegrity(); backend.close();
    if (domain->usage().total != 0U) throw std::runtime_error("normalized patch fault retained resource credit after close");
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"fault_triggered\":" << (triggered ? "true" : "false")
        << ",\"operation_threw\":" << (threw ? "true" : "false") << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null"; else std::cout << fault;
    std::cout << ",\"closed_object_bytes\":" << retained << ",\"passed\":true}\n";
}

void mixedStatementAllocationFaultCase(const std::string &mode, RelocatableStorageBackend &backend, size_t fault) {
    const auto persisted = backend.createLogicalNodes({{{"Seed"}, {{"rank", PropertyValue(0)}}}}).front();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    LogicalGraphMutationBatch seed;
    seed.node_creates = {{{"Original"}, {{"rank", PropertyValue(0)}}},
                         {{"Original"}, {{"rank", PropertyValue(0)}}}};
    const auto first = transaction->stageMutationBatch(std::move(seed));
    if (first.created_node_ids.size() != 2U) throw std::runtime_error("mixed statement seed count changed");
    const std::array<uint64_t, 3U> ids{first.created_node_ids[0], persisted, first.created_node_ids[1]};
    LogicalGraphMutationBatch candidate;
    for (size_t index = 0U; index < ids.size(); ++index) {
        const LogicalGraphMutationEntityReference entity{LogicalGraphMutationEntityKind::LGMK_NODE,
            LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids[index]};
        candidate.property_replacements.push_back({entity, {{"rank", PropertyValue(int64_t{10} + static_cast<int64_t>(index))}}});
        candidate.node_label_replacements.push_back({entity, {"Changed-" + std::to_string(index)}});
    }
    bool threw = false;
    faultTriggered.store(false, std::memory_order_relaxed);
    const auto before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    try { (void)transaction->stageMutationBatch(std::move(candidate)); }
    catch (const std::exception &) { threw = true; }
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const auto attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    const bool triggered = faultTriggered.load(std::memory_order_relaxed);
    if (!triggered && threw) throw std::runtime_error("normal mixed statement allocation probe failed");
    if (transaction->state() != LogicalGraphTransactionState::LGTTS_ACTIVE)
        throw std::runtime_error("mixed statement allocation fault finished its transaction");
    (void)transaction->commit();
    const size_t retained = destroyClosed(transaction);
    backend.close(); backend.open();
    {
        auto snapshot = backend.openLogicalGraphReadSnapshot();
        for (size_t index = 0U; index < ids.size(); ++index) {
            const auto node = snapshot->readNodeShaped(ids[index], {.include_labels = true, .all_properties = true});
            const auto rank = std::ranges::find(node.properties, std::string("rank"), &LogicalGraphPropertyEntry::key);
            const auto expectedRank = threw ? int64_t{0} : int64_t{10} + static_cast<int64_t>(index);
            const std::vector<std::string> expectedLabels{threw ? (index == 1U ? "Seed" : "Original")
                : "Changed-" + std::to_string(index)};
            if (rank == node.properties.end() || rank->value != PropertyValue(expectedRank) || node.labels != expectedLabels)
                throw std::runtime_error("failed mixed statement changed its Property or label prefix");
        }
    }
    (void)backend.verifyIntegrity(); backend.close();
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"fault_triggered\":" << (triggered ? "true" : "false")
        << ",\"operation_threw\":" << (threw ? "true" : "false") << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null";
    else std::cout << fault;
    std::cout << ",\"closed_object_bytes\":" << retained << ",\"passed\":true}\n";
}

void propertyHeapAccountingCase(const std::string &mode) {
    using namespace graph::query::execution::detail;
    size_t cases = 0U, maximumObserved = 0U;
    const auto verify = [&](auto make) {
        // Warm any standard-library process metadata before measuring ownership.
        { auto warm = make(); static_cast<void>(warm); }
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        {
            const auto value = make();
            const auto observed = liveBytes.load(std::memory_order_relaxed) - baseline;
            const auto estimated = propertyHeapBufferBytes(value);
            const auto guarded = propertyBufferEstimates(value, [] {});
            if (estimated < observed) throw std::runtime_error("Property heap accounting underestimates observed standard-allocator requests");
            if (guarded.heapBytes != estimated) throw std::runtime_error("guarded Property heap model disagrees with owning model");
            maximumObserved = (std::max)(maximumObserved, observed); ++cases;
        }
        if (liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("Property heap accounting fixture retained an allocation after destruction");
    };
    verify([] { return PropertyValue(std::string("x")); });
    verify([] { std::string text(8192U, 'q'); text.reserve(32768U); return PropertyValue(std::move(text)); });
    verify([] { PropertyValue::Bytes bytes; bytes.reserve(16384U); bytes.resize(7U); return PropertyValue(std::move(bytes)); });
    verify([] { std::vector<PropertyValue> list; list.reserve(128U); return PropertyValue(std::move(list)); });
    for (const size_t reserve : {size_t{0U}, size_t{16U}, size_t{256U}, size_t{4096U}}) {
        verify([reserve] { PropertyValue::MapType map; if (reserve) map.reserve(reserve); return PropertyValue(std::move(map)); });
        verify([reserve] {
            PropertyValue::MapType map; if (reserve) map.reserve(reserve);
            for (size_t i = 0U; i < 64U; ++i) {
                std::string key = "field-" + std::to_string(i); key.reserve(128U);
                std::vector<PropertyValue> list; list.reserve(9U);
                list.emplace_back(std::string(512U + i, 'v'));
                list.emplace_back(PropertyValue::Bytes(128U + i, std::byte{0x41}));
                map.emplace(std::move(key), PropertyValue(std::move(list)));
            }
            for (size_t i = 0U; i < 48U; ++i) map.erase("field-" + std::to_string(i));
            map.rehash(32U);
            // A fresh copy also audits normalization of legacy bucket capacity.
            auto copied = map;
            return PropertyValue(std::move(copied));
        });
    }
    verify([] {
        PropertyValue::MapType inner; inner.reserve(128U);
        inner.emplace("nested", PropertyValue(std::string(8192U, 'n')));
        std::vector<PropertyValue> list; list.reserve(64U); list.emplace_back(std::move(inner));
        PropertyValue::MapType outer; outer.reserve(256U); outer.emplace("children", PropertyValue(std::move(list)));
        return PropertyValue(std::move(outer));
    });
    const auto verifyRow = [&](auto make) {
        { auto warm = make(); static_cast<void>(warm); }
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        {
            const auto row = make();
            const auto observed = liveBytes.load(std::memory_order_relaxed) - baseline;
            if (graph::query::detail::resultRowHeapBytes(row) < observed)
                throw std::runtime_error("result row heap accounting underestimates observed standard-allocator requests");
            maximumObserved = (std::max)(maximumObserved, observed); ++cases;
        }
        if (liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("result row accounting fixture retained an allocation after destruction");
    };
    verifyRow([] { graph::query::QueryResult::Row row; row.reserve(4096U); return row; });
    verifyRow([] {
        graph::query::QueryResult::Row row; row.reserve(128U);
        graph::query::LogicalNodeResult node; node.node_id = 7U;
        node.labels.reserve(128U); node.labels.emplace_back("Short"); node.labels.emplace_back(std::string(4096U, 'l'));
        node.properties.reserve(128U); node.properties.emplace("body", PropertyValue(std::string(8192U, 'b')));
        row.emplace("node", graph::query::ResultValue(std::move(node)));
        row.emplace("text", graph::query::ResultValue(PropertyValue(std::string("x"))));
        return row;
    });
    verifyRow([] {
        graph::query::QueryResult::Row row; row.reserve(256U);
        for (size_t i = 0U; i < 16U; ++i)
            row.emplace(std::string(80U, 'a') + std::to_string(i),
                graph::query::ResultValue(PropertyValue(PropertyValue::Bytes(16384U + i, std::byte{0x52}))));
        return row;
    });
    const auto verifyInput = [&](auto make) {
        const auto source = make();
        auto budget = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        {
            auto copy = graph::storage::detail::AdmittedMutationInput::copy(budget, source.first, source.second);
            const auto observed = liveBytes.load(std::memory_order_relaxed) - baseline;
            if (budget->used() < observed)
                throw std::runtime_error("private mutation copy admission underestimated allocator requests");
            maximumObserved = (std::max)(maximumObserved, observed); ++cases;
        }
        if (budget->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("private mutation copy did not free data before refunding its owner");
    };
    using Input = std::pair<LogicalGraphMutationBatch, std::vector<LogicalGraphPropertyPatchRequest>>;
    verifyInput([] { return Input{}; });
    verifyInput([] {
        Input input;
        PropertyValue::MapType map; map.reserve(4096U);
        map.emplace("text", PropertyValue(std::string(8192U, 'n')));
        map.emplace("bytes", PropertyValue(PropertyValue::Bytes(16384U, std::byte{0x41})));
        input.first.node_creates.push_back({{std::string(4096U, 'l')}, {{"nested", PropertyValue(std::move(map))}}});
        return input;
    });
    verifyInput([] {
        Input input; LogicalGraphBatchEdgeCreateRequest edge;
        edge.type = std::string(8192U, 't'); edge.type.reserve(32768U);
        edge.properties.push_back({"bytes", PropertyValue(PropertyValue::Bytes(8192U, std::byte{0x42}))});
        input.first.edge_creates.push_back(std::move(edge)); return input;
    });
    verifyInput([] {
        Input input; LogicalGraphPropertyMutationRequest request;
        std::vector<PropertyValue> values(128U, PropertyValue(std::string(80U, 'p')));
        request.properties.push_back({"list", PropertyValue(std::move(values))});
        input.first.property_replacements.push_back(std::move(request)); return input;
    });
    verifyInput([] {
        Input input; LogicalGraphNodeLabelMutationRequest request;
        request.labels.resize(128U, std::string(80U, 'l'));
        input.first.node_label_replacements.push_back(std::move(request));
        LogicalGraphPropertyPatchRequest patch;
        patch.sets.push_back({"field", PropertyValue(std::string(8192U, 's'))});
        patch.removals.resize(64U, std::string(80U, 'r'));
        input.second.push_back(std::move(patch)); return input;
    });
    verifyInput([] {
        Input input; input.first.node_deletes.resize(10000U); input.first.edge_deletes.resize(10000U);
        return input;
    });
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_observed_requested_bytes\":" << maximumObserved << ",\"passed\":true}\n";
}

void sourceMaterializationReadFaultCase(const std::string &mode, const char *path, size_t fault) {
    RelocatableStorageBackendOptions options; options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    const PropertyValue expected(std::string(65536U, 'r'));
    const auto nodes = backend->createLogicalNodes({{{"ReadFault"}, {{"payload", expected}}}, {{}, {}}});
    const auto edge = backend->createLogicalEdges({{nodes[0], nodes[1], "ReadFault", {{"payload", expected}}}}).front();
    const auto fence = backend->currentNativeLogicalGraphMutationFence();
    auto domain = backend->queryResourceDomain();
    auto snapshot = backend->openLogicalGraphReadSnapshot();
    bool threw = false;
    faultTriggered.store(false, std::memory_order_relaxed);
    const auto before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    std::optional<LogicalGraphNode> observedNode; std::optional<LogicalGraphEdge> observedEdge;
    try {
        observedNode = snapshot->readNodeShaped(nodes[0], {.include_labels = true, .all_properties = true});
        observedEdge = snapshot->readEdgeShaped(edge, {.include_type = true, .all_properties = true});
    } catch (const std::bad_alloc &) { threw = true; }
    catch (...) { faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed); throw; }
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const auto attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    const auto triggered = faultTriggered.load(std::memory_order_relaxed);
    if (!triggered && threw) throw std::runtime_error("normal complete-source read allocation probe failed");
    const auto validate = [&](const LogicalGraphNode &node, const LogicalGraphEdge &relationship) {
        if (node.node_id != nodes[0] || node.labels != std::vector<std::string>{"ReadFault"} ||
            node.properties.size() != 1U || node.properties[0].value != expected ||
            relationship.edge_id != edge || relationship.type != "ReadFault" ||
            relationship.properties.size() != 1U || relationship.properties[0].value != expected)
            throw std::runtime_error("source read allocation fault changed a successful complete payload");
    };
    // Successful-result assertions stay outside exception capture.
    if (!threw) {
        if (!observedNode || !observedEdge) throw std::runtime_error("source read returned an incomplete success");
        validate(*observedNode, *observedEdge);
    }
    const auto retryNode = snapshot->readNodeShaped(nodes[0], {.include_labels = true, .all_properties = true});
    const auto retryEdge = snapshot->readEdgeShaped(edge, {.include_type = true, .all_properties = true});
    validate(retryNode, retryEdge);
    if (backend->hasIndeterminateWriteOutcome() || backend->currentNativeLogicalGraphMutationFence() != fence)
        throw std::runtime_error("read allocation failure changed the publication fence");
    snapshot.reset(); static_cast<void>(backend->verifyIntegrity()); backend->close();
    if (domain->usage().total != 0U) throw std::runtime_error("closed source read retained resource credit");
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"fault_triggered\":" << (triggered ? "true" : "false") << ",\"operation_threw\":" << (threw ? "true" : "false")
        << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null"; else std::cout << fault;
    std::cout << ",\"passed\":true}\n";
}

void sourceMaterializationCopyCase(const std::string &mode, const char *path) {
    captureBodyAllocation.store(true, std::memory_order_relaxed);
    { const std::string prototype(65536U, 'p'); static_cast<void>(prototype.data()); }
    captureBodyAllocation.store(false, std::memory_order_relaxed);
    const auto request = prototypeBodyRequest.load(std::memory_order_relaxed);
    if (request == 0U) throw std::runtime_error("could not observe the standard string allocation request");
    bodyAllocationRequest.store(request, std::memory_order_relaxed);
    const PropertyValue expected(std::string(65536U, 'p'));
    RelocatableStorageBackendOptions options; options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    const auto nodes = backend->createLogicalNodes({{{}, {{"payload", expected}}}, {{}, {}}});
    const auto edge = backend->createLogicalEdges({{nodes[0], nodes[1], "CopyProbe", {{"payload", expected}}}}).front();
    auto snapshot = backend->openLogicalGraphReadSnapshot();
    bodyAllocationMatches.store(0U, std::memory_order_relaxed);
    captureBodyAllocation.store(true, std::memory_order_relaxed);
    LogicalGraphNode node;
    try { node = snapshot->readNodeShaped(nodes[0], {.all_properties = true}); }
    catch (...) { captureBodyAllocation.store(false, std::memory_order_relaxed); throw; }
    captureBodyAllocation.store(false, std::memory_order_relaxed);
    const auto nodeAllocations = bodyAllocationMatches.load(std::memory_order_relaxed);
    if (node.properties.size() != 1U || node.properties[0].value != expected)
        throw std::runtime_error("move materialization changed the complete Node payload");
    bodyAllocationMatches.store(0U, std::memory_order_relaxed);
    captureBodyAllocation.store(true, std::memory_order_relaxed);
    LogicalGraphEdge relationship;
    try { relationship = snapshot->readEdgeShaped(edge, {.include_type = true, .all_properties = true}); }
    catch (...) { captureBodyAllocation.store(false, std::memory_order_relaxed); throw; }
    captureBodyAllocation.store(false, std::memory_order_relaxed);
    const auto edgeAllocations = bodyAllocationMatches.load(std::memory_order_relaxed);
    if (relationship.properties.size() != 1U || relationship.properties[0].value != expected || relationship.type != "CopyProbe")
        throw std::runtime_error("move materialization changed the complete Edge payload");
    if (nodeAllocations != 1U || edgeAllocations != 1U)
        throw std::runtime_error("source materialization repeated large string allocations: node=" + std::to_string(nodeAllocations) +
            ", edge=" + std::to_string(edgeAllocations));
    snapshot.reset(); static_cast<void>(backend->verifyIntegrity()); backend->close();
    std::cout << "{\"mode\":\"" << mode << "\",\"string_request_bytes\":" << request
        << ",\"node_body_allocations\":" << nodeAllocations << ",\"edge_body_allocations\":" << edgeAllocations
        << ",\"passed\":true}\n";
}

void schemaCacheAllocationFaultCase(const std::string &mode, size_t fault) {
    using namespace graph::query::execution::detail;
    constexpr size_t capacity = 4U * 1024U * 1024U;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(capacity));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(capacity));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, capacity);
    auto memory = std::make_shared<PrepaidMemoryResource>(account, capacity);
    std::vector<graph::storage::IndexDefinition> original(1U), replacement(3U);
    original.front().name = "old";
    for (size_t index = 0U; index < replacement.size(); ++index) {
        replacement[index].name = std::string(4096U, static_cast<char>('a' + index));
        replacement[index].property_names.emplace_back(std::string(1024U, 'p'));
    }
    size_t attempts = 0U; bool failed = false;
    {
        graph::storage::NativeReadSession session(memory); session.putIndexDefinitions(original);
        const auto baseline = memory->used();
        allocationAttempts.store(0U); faultTriggered.store(false); faultRemaining.store(fault);
        try { session.putIndexDefinitions(replacement); }
        catch (const std::bad_alloc &) { failed = true; }
        catch (...) { faultRemaining.store((std::numeric_limits<size_t>::max)()); throw; }
        faultRemaining.store((std::numeric_limits<size_t>::max)());
        attempts = allocationAttempts.load();
        const auto *actual = session.indexDefinitions(64U);
        if (!actual) throw std::runtime_error("schema cache lost its validated state");
        if (failed) {
            if (*actual != original || memory->used() != baseline)
                throw std::runtime_error("failed schema cache copy changed old data or retained partial credit");
        } else if (*actual != replacement) throw std::runtime_error("schema cache copy changed successful content");
    }
    if (memory->used() != 0U) throw std::runtime_error("schema cache fault left retained credit");
    std::cout << "{\"mode\":\"" << mode << "\",\"selected_fault\":" << fault
        << ",\"fault_triggered\":" << (faultTriggered.load() ? "true" : "false")
        << ",\"allocation_attempts\":" << attempts << ",\"passed\":true}\n";
}

template<class Definition, class Codec>
void ownedSchemaDefinitionHeapAccountingCase(const std::string &mode) {
    using namespace graph::storage;
    size_t cases = 0U;
    for (const size_t length : {1U, 15U, 16U, 31U, 1024U, 4096U}) {
        Definition prototype;
        prototype.name = std::string(length, 'n');
        if constexpr (std::is_same_v<Definition, IndexDefinition>) {
            prototype.index_id = 1U;
            prototype.kind = length == 4096U ? IndexDefinitionKind::IDK_COMPOSITE : IndexDefinitionKind::IDK_PROPERTY;
            prototype.label_token_id = 1U;
        } else {
            prototype.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
            prototype.kind = length == 4096U ? ConstraintDefinitionKind::CDK_NODE_KEY : ConstraintDefinitionKind::CDK_UNIQUE;
            prototype.schema_token_id = 1U;
        }
        const size_t properties = length == 4096U ? 64U : 1U;
        for (size_t index = 0U; index < properties; ++index)
            prototype.property_names.emplace_back(std::string(length == 4096U ? 1000U : 16U, 'p') + std::to_string(index));
        const auto encoded = Codec::encode(prototype);
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        std::optional<Definition> actual;
        try {
            graph::utils::OwnedAllocationScope scope(memory);
            actual.emplace(Codec::decode(encoded));
            {
                graph::utils::OwnedStdVector<Definition> values;
                values.push_back(*actual); values.reserve(8U);
                graph::utils::OwnedStdVector<Definition> copied(values);
            }
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (*actual != prototype) throw std::runtime_error("owned index definition changed decoded content");
        actual.reset();
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("owned index definition underestimated requests: " + std::to_string(maximumUncoveredWriteBytes));
        if (memory->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("owned index definition retained credit or storage");
        {
            using namespace graph::query::execution::detail;
            const std::vector<Definition> input{prototype};
            const std::vector<Definition> empty;
            constexpr size_t capacity = 4U * 1024U * 1024U;
            auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(capacity));
            auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(capacity));
            auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, capacity);
            auto cache = std::make_shared<PrepaidMemoryResource>(account, capacity);
            const auto cacheBaseline = liveBytes.load(std::memory_order_relaxed);
            observedInitialLive = cacheBaseline; observedInitialCharge = 0U;
            maximumUncoveredWriteBytes = 0U; observedPrepaidBudget = cache.get();
            try {
                {
                    NativeReadSession session(cache);
                    if constexpr (std::is_same_v<Definition, IndexDefinition>) {
                        session.putIndexDefinitions(input);
                        if (session.indexDefinitions(64U) == nullptr) throw std::runtime_error("owned index cache bypassed a valid shape");
                        session.putIndexDefinitions(empty);
                    } else {
                        session.putConstraintDefinitions(input);
                        if (session.constraintDefinitions(64U) == nullptr) throw std::runtime_error("owned constraint cache bypassed a valid shape");
                        session.putConstraintDefinitions(empty);
                    }
                }
            } catch (...) { observedPrepaidBudget = nullptr; throw; }
            observedPrepaidBudget = nullptr;
            if (maximumUncoveredWriteBytes != 0U)
                throw std::runtime_error("schema cache underestimated requests: " + std::to_string(maximumUncoveredWriteBytes));
            if (cache->used() != 0U || liveBytes.load(std::memory_order_relaxed) != cacheBaseline)
                throw std::runtime_error("schema cache retained credit or storage");
        }
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void resultColumnCopyAdmissionCase(const std::string &mode) {
    using namespace graph::query::execution::detail;
    const std::vector<std::string> input{std::string(65536U, 'c')};
    bodyAllocationRequest.store(0U); prototypeBodyRequest.store(0U);
    captureBodyAllocation.store(true);
    { const std::vector<std::string> prototype(input); }
    captureBodyAllocation.store(false);
    const auto request = prototypeBodyRequest.load();
    if (request == 0U) throw std::runtime_error("column-copy prototype did not allocate its character buffer");
    for (const auto maximum : {4096U, 262144U}) {
        auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
        auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
        auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, maximum);
        {
            graph::query::QueryResult result{ResourceLease(account)};
            result.setColumns({"old"});
            const auto previous = account->used();
            bodyAllocationRequest.store(request); bodyAllocationMatches.store(0U); captureBodyAllocation.store(true);
            bool rejected = false;
            try { result.setColumns(input); }
            catch (const ResourceAdmissionDenied &) { rejected = true; }
            catch (...) { captureBodyAllocation.store(false); throw; }
            captureBodyAllocation.store(false);
            if (maximum == 4096U) {
                if (!rejected || bodyAllocationMatches.load() != 0U || result.getColumns().front() != "old" || account->used() != previous)
                    throw std::runtime_error("column-copy rejection allocated payload or changed old output");
            } else {
                if (rejected || bodyAllocationMatches.load() != 1U || result.getColumns() != input)
                    throw std::runtime_error("admitted column copy changed allocation count or content");
                const auto retained = account->used();
                bodyAllocationMatches.store(0U); captureBodyAllocation.store(true);
                try { result.setColumns(result.getColumns()); }
                catch (...) { captureBodyAllocation.store(false); throw; }
                captureBodyAllocation.store(false);
                if (bodyAllocationMatches.load() != 0U || account->used() != retained)
                    throw std::runtime_error("column-copy self assignment changed storage or credit");
            }
        }
        if (account->used() != 0U) throw std::runtime_error("column-copy retained result credit");
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":3,\"rejected_payload_allocations\":0,\"accepted_payload_allocations\":1,\"passed\":true}\n";
}

void queryContextAccountValidationCase(const std::string &mode, const std::filesystem::path &path) {
    using namespace graph::query;
    using namespace graph::query::execution::detail;
    RelocatableStorageBackendOptions options;
    options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    QueryEngine engine(std::static_pointer_cast<LogicalGraphReadBackend>(backend),
        std::static_pointer_cast<LogicalGraphWriteBackend>(backend));
    auto transaction = backend->beginLogicalGraphTransaction(true);
    LogicalGraphTransactionOverlay overlay(*transaction);
    QueryContext context;
    context.parameters.emplace("payload", PropertyValue(std::string(65536U, 'p')));
    bodyAllocationRequest.store(0U); prototypeBodyRequest.store(0U); captureBodyAllocation.store(true);
    { QueryContext prototype(context); }
    captureBodyAllocation.store(false);
    const auto request = prototypeBodyRequest.load();
    if (request == 0U) throw std::runtime_error("context prototype did not copy its parameter payload");
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(1048576U));
    auto foreign = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(1048576U));
    size_t cases = 0U;
    for (const bool wrongClass : {false, true}) {
        context.resource_account = std::make_shared<ResourceAccount>(
            wrongClass ? backend->queryResourceDomain() : foreign,
            wrongClass ? ResourceClass::RCK_CACHE : ResourceClass::RCK_QUERY, 65536U);
        for (int entry = 0; entry < 3; ++entry) {
            auto plan = engine.buildPlan("RETURN 42 AS value");
            bodyAllocationRequest.store(request); bodyAllocationMatches.store(0U); captureBodyAllocation.store(true);
            bool rejected = false;
            try {
                if (entry == 0) (void)engine.execute("RETURN 42 AS value", context);
                else if (entry == 1) (void)engine.executePlan(std::move(plan), context);
                else (void)engine.executePlanWithLogicalTransactionOverlay(std::move(plan), context, overlay, overlay);
            } catch (const std::invalid_argument &) { rejected = true; }
            catch (...) { captureBodyAllocation.store(false); throw; }
            captureBodyAllocation.store(false);
            if (!rejected || bodyAllocationMatches.load() != 0U || context.resource_account->used() != 0U)
                throw std::runtime_error("invalid query account copied parameter payload before rejection");
            ++cases;
        }
    }
    for (int entry = 0; entry < 3; ++entry) {
        auto plan = engine.buildPlan("RETURN $payload AS value");
        context.resource_account = std::make_shared<ResourceAccount>(backend->queryResourceDomain(),
            ResourceClass::RCK_QUERY, 4096U);
        bodyAllocationRequest.store(request); bodyAllocationMatches.store(0U); captureBodyAllocation.store(true);
        bool rejected = false;
        try {
            if (entry == 0) (void)engine.execute("RETURN $payload AS value", context);
            else if (entry == 1) (void)engine.executePlan(std::move(plan), context);
            else (void)engine.executePlanWithLogicalTransactionOverlay(std::move(plan), context, overlay, overlay);
        } catch (const ResourceAdmissionDenied &) { rejected = true; }
        catch (...) { captureBodyAllocation.store(false); throw; }
        captureBodyAllocation.store(false);
        if (!rejected || bodyAllocationMatches.load() != 0U || context.resource_account->used() != 0U)
            throw std::runtime_error("context quota rejection copied payload or retained parameter credit");
        ++cases;
    }
    context.resource_account = std::make_shared<ResourceAccount>(backend->queryResourceDomain(),
        ResourceClass::RCK_QUERY, 1048576U);
    {
        const auto result = engine.execute("RETURN $payload AS value", context);
        if (result.rowCount() != 1U || result.getRows()[0].at("value").asPrimitive() != context.parameters.at("payload"))
            throw std::runtime_error("admitted parameter context changed output");
    }
    if (context.resource_account->used() != 0U)
        throw std::runtime_error("successful context copy retained its parameter credit");
    ++cases;
    transaction->rollback(); engine.resetEphemeralState(); backend->close();
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"rejected_payload_allocations\":0,\"passed\":true}\n";
}

void ownedPlanHeapAccountingCase(const std::string &mode) {
    using namespace graph::query::logical;
    using namespace graph::query::expressions;
    std::vector<std::unique_ptr<LogicalOperator>> prototypes;
    std::vector<LogicalProjectItem> items;
    for (size_t index = 0U; index < 64U; ++index)
        items.emplace_back(graph::utils::makeSharedOwned<LiteralExpression>(std::string(512U, 's')),
            std::string(80U, 'a') + std::to_string(index));
    auto project = std::make_unique<LogicalProject>(std::make_unique<LogicalSingleRow>(), std::move(items));
    std::unordered_set<std::string> required;
    for (size_t index = 0U; index < 64U; ++index) required.insert(std::string(80U, 'r') + std::to_string(index));
    project->setRequiredColumns(std::move(required));
    prototypes.push_back(std::move(project));
    std::vector<LogicalSortItem> sortItems;
    for (size_t index = 0U; index < 64U; ++index)
        sortItems.emplace_back(graph::utils::makeSharedOwned<VariableReferenceExpression>(std::string(80U, 'v')), index % 2U == 0U);
    prototypes.push_back(std::make_unique<LogicalSort>(std::make_unique<LogicalSingleRow>(), std::move(sortItems)));
    std::vector<LogicalAggItem> aggregations;
    std::vector<std::shared_ptr<Expression>> group;
    group.push_back(graph::utils::makeSharedOwned<VariableReferenceExpression>("group"));
    for (size_t index = 0U; index < 64U; ++index)
        aggregations.emplace_back("collect", graph::utils::makeSharedOwned<LiteralExpression>(std::string(512U, 's')),
            std::string(80U, 'a') + std::to_string(index));
    prototypes.push_back(std::make_unique<LogicalAggregate>(std::make_unique<LogicalSingleRow>(), std::move(group),
        std::move(aggregations), std::vector<std::string>{std::string(80U, 'g')}));
    graph::PropertyValue::MapType nested;
    nested.rehash(4096U);
    nested.emplace(std::string(80U, 'k'), graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(std::string(8192U, 'v'))}));
    const graph::PropertyValue payload(nested);
    const std::vector<std::pair<std::string, graph::PropertyValue>> predicates{{std::string(80U, 'k'), payload}};
    auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{std::string(80U, 'l')}, predicates);
    scan->setRangePredicates({RangePredicate{std::string(80U, 'r'), payload, payload, true, false}});
    scan->setParameterPropertyPredicates({ParameterPropertyPredicate{std::string(80U, 'k'), std::string(80U, 'p')}});
    scan->setCompositeEquality(CompositeEqualityPredicate{{std::string(80U, 'k')}, {payload}});
    prototypes.push_back(std::move(scan));
    prototypes.push_back(std::make_unique<LogicalTraversal>(std::make_unique<LogicalSingleRow>(), "s", "e", "t", "T", "OUT",
        std::vector<std::string>{}, predicates, nested));
    prototypes.push_back(std::make_unique<LogicalUnwind>(std::make_unique<LogicalSingleRow>(), "v", std::vector<graph::PropertyValue>{payload}));
    std::unordered_map<std::string, std::shared_ptr<Expression>> expressionMap;
    std::vector<LogicalSetItem> assignments;
    std::vector<MergeSetAction> actions;
    for (size_t index = 0U; index < 64U; ++index) {
        auto expression = graph::utils::makeSharedOwned<LiteralExpression>(std::string(512U, 'v'));
        expressionMap.emplace(std::string(80U, 'k') + std::to_string(index), expression);
        assignments.push_back({SetActionType::LSET_PROPERTY, std::string(80U, 'n'), std::string(80U, 'k'), expression});
        actions.push_back({std::string(80U, 'n'), std::string(80U, 'k'), expression, SetActionType::LSET_PROPERTY});
    }
    prototypes.push_back(std::make_unique<LogicalCreateNode>("n", std::vector<std::string>{}, nested, expressionMap));
    prototypes.push_back(std::make_unique<LogicalSet>(std::move(assignments)));
    prototypes.push_back(std::make_unique<LogicalMergeNode>("n", std::vector<std::string>{}, nested, actions, actions,
        nullptr, expressionMap));
    std::vector<LogicalRemoveItem> removals;
    for (size_t index = 0U; index < 64U; ++index)
        removals.push_back({LogicalRemoveActionType::LREM_PROPERTY, std::string(80U, 'n'), std::string(80U, 'k')});
    prototypes.push_back(std::make_unique<LogicalRemove>(std::move(removals)));
    size_t cases = 0U;
    for (const auto &prototype : prototypes) {
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
        const auto expectedColumns = prototype->getOutputVariables();
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        std::unique_ptr<LogicalOperator> actual;
        try {
            graph::utils::OwnedAllocationScope scope(memory);
            actual = prototype->clone();
            {
                auto columns = actual->getOwnedOutputVariables();
                observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
                if (static_cast<const std::vector<std::string> &>(columns) != expectedColumns)
                    throw std::runtime_error("owned output metadata changed column count");
            }
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (actual->toString() != prototype->toString()) throw std::runtime_error("owned plan clone changed content");
        actual.reset();
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("owned plan underestimated allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
        if (memory->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("owned plan retained storage or credit");
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void ownedStringContainersHeapAccountingCase(const std::string &mode) {
    size_t cases = 0U;
    for (const size_t count : {0U, 1U, 15U, 16U, 31U, 4096U, 8192U}) {
        const std::string source(count, 's');
        const std::unordered_set<std::string> inputKeys{source};
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        try {
            {
                graph::utils::OwnedAllocationScope scope(memory);
                graph::utils::OwnedStringVector values;
                values.push_back(source); values.push_back(values[0]);
                values.emplace_back(source);
                values.reserve(64U);
                graph::utils::OwnedStringVector copied(values);
                copied.emplace_back(copied[0]);
                copied.reserveString(0U, 8192U); copied.pop_back(); copied.clear();
                graph::utils::OwnedStringSet keys(inputKeys);
                graph::utils::OwnedStringSet second(keys);
                graph::utils::OwnedStringSet moved(std::move(second));
                if (moved.count(source) != 1U) throw std::runtime_error("owned set lost its key");
            }
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("owned string containers underestimated allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
        if (memory->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("owned string containers retained storage or credit");
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void ownedPropertyHeapAccountingCase(const std::string &mode) {
    std::vector<PropertyValue> prototypes{PropertyValue(), PropertyValue(int64_t{7}),
        PropertyValue(std::string(8192U, 's')), PropertyValue(PropertyValue::Bytes(16384U, std::byte{0x31})),
        PropertyValue(std::vector<PropertyValue>(1024U, PropertyValue(std::string(512U, 'l'))))};
    for (const size_t count : {0U, 1U, 13U, 257U}) {
        PropertyValue::MapType map;
        for (size_t index = 0U; index < count; ++index)
            map.emplace(std::string(80U, 'k') + std::to_string(index),
                PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(80U, 'v')),
                    PropertyValue(PropertyValue::Bytes(128U, std::byte{0x32}))}));
        prototypes.emplace_back(std::move(map));
    }
    PropertyValue::MapType sparse;
    sparse.rehash(4096U); sparse.emplace("one", PropertyValue(std::string(8192U, 's')));
    prototypes.emplace_back(std::move(sparse));
    size_t cases = 0U;
    for (const auto &expected : prototypes) {
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        try {
            {
                graph::utils::OwnedAllocationScope scope(memory);
                const auto listSource = [&](size_t index) { return graph::property_heap::CopySource(expected.getList()[index]); };
                graph::OwnedPropertyValue actual = expected.getType() == PropertyType::LIST
                    ? graph::OwnedPropertyValue::fromList(expected.getList().size(), listSource)
                    : graph::OwnedPropertyValue(expected);
                graph::OwnedPropertyValue copy(actual);
                if (actual.get() != expected || copy.get() != expected)
                    throw std::runtime_error("owned Property clone changed content");
                copy = PropertyValue(int64_t{42});
                actual = PropertyValue();
                if (copy.get() != PropertyValue(int64_t{42})) throw std::runtime_error("owned Property replacement changed content");
            }
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("owned Property underestimated allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
        if (memory->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("owned Property retained storage or credit");
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void ownedExpressionHeapAccountingCase(const std::string &mode) {
    using namespace graph::query::expressions;
    std::vector<std::unique_ptr<Expression>> prototypes;
    prototypes.push_back(std::make_unique<LiteralExpression>());
    prototypes.push_back(std::make_unique<LiteralExpression>(int64_t{7}));
    prototypes.push_back(std::make_unique<LiteralExpression>(std::string(8192U, 's')));
    prototypes.push_back(std::make_unique<VariableReferenceExpression>(std::string(8192U, 'v'), std::string(4096U, 'p')));
    prototypes.push_back(std::make_unique<ParameterExpression>(std::string(8192U, 'p')));
    prototypes.push_back(std::make_unique<BinaryOpExpression>(std::make_unique<VariableReferenceExpression>("v"),
        BinaryOperatorType::BOP_ADD, std::make_unique<LiteralExpression>(std::string(8192U, 's'))));
    std::vector<std::unique_ptr<Expression>> arguments;
    for (size_t index = 0U; index < 64U; ++index)
        arguments.push_back(std::make_unique<LiteralExpression>(std::string(512U, 'a')));
    prototypes.push_back(std::make_unique<FunctionCallExpression>(std::string(8192U, 'f'), std::move(arguments)));
    std::vector<std::unique_ptr<Expression>> dynamicElements;
    for (size_t index = 0U; index < 64U; ++index)
        dynamicElements.push_back(std::make_unique<VariableReferenceExpression>(std::string(80U, 'v') + std::to_string(index)));
    prototypes.push_back(std::make_unique<ListLiteralExpression>(std::move(dynamicElements)));
    prototypes.push_back(std::make_unique<ListLiteralExpression>(PropertyValue(
        std::vector<PropertyValue>(64U, PropertyValue(std::string(512U, 's'))))));
    auto conditional = std::make_unique<CaseExpression>();
    for (size_t index = 0U; index < 64U; ++index)
        conditional->addBranch(std::make_unique<LiteralExpression>(true),
            std::make_unique<LiteralExpression>(std::string(512U, 'c')));
    prototypes.push_back(std::move(conditional));
    graph::utils::OwnedStdVector<MapProjectionItem> mapItems;
    for (size_t index = 0U; index < 64U; ++index)
        mapItems.emplace_back(MapProjectionItemType::MPROP_LITERAL, std::string(80U, 'k') + std::to_string(index),
            std::make_unique<LiteralExpression>(std::string(512U, 'm')));
    prototypes.push_back(std::make_unique<MapProjectionExpression>(std::move(mapItems)));
    prototypes.push_back(std::make_unique<InExpression>(std::make_unique<LiteralExpression>(int64_t{1}),
        std::vector<PropertyValue>(64U, PropertyValue(std::string(512U, 'i')))));
    size_t cases = 0U;
    for (const auto &prototype : prototypes) {
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        std::shared_ptr<Expression> actual;
        try {
            graph::utils::OwnedAllocationScope scope(memory);
            actual = graph::utils::shareOwned(prototype->clone());
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        // Rendering is a separate caller-owned API operation, outside this
        // expression-clone/control-block allocation probe.
        if (actual->toString() != prototype->toString()) throw std::runtime_error("owned expression clone changed content");
        actual.reset();
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("owned expression underestimated allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
        if (memory->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("owned expression retained storage or credit");
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void ownedStdStringHeapAccountingCase(const std::string &mode) {
    size_t cases = 0U;
    for (const size_t count : {0U, 1U, 15U, 16U, 22U, 30U, 31U, 32U, 4095U, 4096U, 8192U, 65536U}) {
        const std::string source(count, 's');
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        try {
            {
                graph::utils::OwnedAllocationScope scope(memory);
                graph::utils::OwnedStdString value(source);
                graph::utils::OwnedStdString copy(value);
                {
                    graph::utils::OwnedStdString moved(std::move(copy));
                    if (static_cast<const std::string &>(moved) != source) throw std::runtime_error("moved string changed content");
                }
                copy = source;
                value = "replacement";
                if (static_cast<const std::string &>(copy) != source || value != "replacement")
                    throw std::runtime_error("owned string replacement changed content");
            }
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("owned string underestimated allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
        if (memory->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("owned string retained storage or credit");
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void admittedCodecHeapAccountingCase(const std::string &mode) {
    using Codec = CanonicalPropertyValueCodec;
    const bool scoped = mode == "codec-scoped-heap-accounting";
    const bool guarded = mode == "guarded-copy-heap-accounting";
    size_t cases = 0U, maximumObserved = 0U;
    const auto verify = [&](const PropertyValue &expected) {
        std::vector<std::byte> encoded; Codec::append(encoded, expected);
        auto budget = std::make_shared<graph::query::execution::MaterializedByteBudget>(16U * 1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        graph::query::execution::MaterializedByteReservation charge(budget.get());
        const std::function<void(const Codec::DecodeShape &)> admit = [&](const auto &shape) {
            charge.set(shape.maximum_heap_request_bytes);
        };
        // Callback construction is caller-owned, preceding the measurement.
        const auto liveStart = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = liveStart; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = budget.get();
        size_t offset = 0U;
        try {
            std::optional<graph::storage::detail::PropertyDecodeWorkspace> workspace;
            if (scoped) workspace.emplace(budget);
            {
                if (guarded) charge.set(Codec::guardedCopyHeapUpper(expected));
                const auto actual = guarded ? graph::property_copy::copyPropertyValueGuarded(expected, [] {})
                    : scoped ? Codec::decodeNext(encoded, offset)
                    : Codec::decodeNextWithAdmission(encoded, offset, admit);
                observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
                if (actual != expected || (!guarded && offset != encoded.size())) {
                    observedWriteBudget = nullptr;
                    throw std::runtime_error("admitted codec changed its complete decoded value");
                }
                maximumObserved = (std::max)(maximumObserved,
                    liveBytes.load(std::memory_order_relaxed) - liveStart);
            }
            workspace.reset();
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("admitted codec underestimated allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
        charge.reset();
        if (budget->used() != 0U || liveBytes.load(std::memory_order_relaxed) != liveStart)
            throw std::runtime_error("admitted codec retained decoded buffers or credit");
        static_cast<void>(baseline); ++cases;
    };
    verify(PropertyValue()); verify(PropertyValue(std::string("small")));
    verify(PropertyValue(std::string(8192U, 's')));
    if (guarded) for (const size_t count : {16U, 22U, 30U, 31U, 32U}) verify(PropertyValue(std::string(count, 's')));
    verify(PropertyValue(PropertyValue::Bytes(16384U, std::byte{0x42})));
    verify(PropertyValue(std::vector<PropertyValue>(1024U, PropertyValue(std::string(512U, 'l')))));
    for (const size_t count : {size_t{0U}, size_t{1U}, size_t{13U}, size_t{257U}, size_t{1111U}}) {
        PropertyValue::MapType map;
        for (size_t i = 0U; i < count; ++i)
            map.emplace(std::string(i % 2U == 0U ? 80U : 4U, 'k') + std::to_string(i),
                PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(80U, 'v')),
                    PropertyValue(PropertyValue::Bytes(128U, std::byte{0x43}))}));
        verify(PropertyValue(std::move(map)));
    }
    if (guarded) for (const auto factor : {0.01F, 1000.0F}) {
        PropertyValue::MapType sparse;
        for (size_t i = 0U; i < 257U; ++i) sparse.emplace(std::to_string(i), PropertyValue(std::string(22U, 'm')));
        sparse.max_load_factor(factor);
        verify(PropertyValue(std::move(sparse)));
    }
    PropertyValue::MapType inner; inner.emplace("bytes", PropertyValue(PropertyValue::Bytes(4096U, std::byte{0x44})));
    PropertyValue::MapType outer; outer.emplace("child", PropertyValue(std::move(inner)));
    verify(PropertyValue(std::move(outer)));
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"maximum_observed_requested_bytes\":" << maximumObserved
        << ",\"passed\":true}\n";
}

void scopedDecodePartialCancellationCase(const std::string &mode) {
    const PropertyValue expected(std::vector<PropertyValue>(1024U, PropertyValue(std::string(512U, 'c'))));
    std::vector<std::byte> encoded; CanonicalPropertyValueCodec::append(encoded, expected);
    auto budget = std::make_shared<graph::query::execution::MaterializedByteBudget>(1048576U);
    struct Cancelled {};
    bool allocatedBeforeCancellation = false, cancelled = false;
    size_t initialLive = 0U;
    const std::function<void()> checkpoint = [&] {
        if (liveBytes.load(std::memory_order_relaxed) > initialLive + 65536U) {
            allocatedBeforeCancellation = true; throw Cancelled{};
        }
    };
    initialLive = liveBytes.load(std::memory_order_relaxed);
    observedInitialLive = initialLive; observedInitialCharge = 0U; maximumUncoveredWriteBytes = 0U;
    observedWriteBudget = budget.get(); size_t offset = 0U;
    try {
        graph::storage::detail::PropertyDecodeWorkspace workspace(budget, &checkpoint);
        static_cast<void>(CanonicalPropertyValueCodec::decodeNext(encoded, offset));
    } catch (const Cancelled &) { cancelled = true; }
    catch (...) { observedWriteBudget = nullptr; throw; }
    observeWriteOwnership(liveBytes.load(std::memory_order_relaxed)); observedWriteBudget = nullptr;
    if (!cancelled || !allocatedBeforeCancellation || offset != 0U || budget->used() != 0U ||
        liveBytes.load(std::memory_order_relaxed) != initialLive || maximumUncoveredWriteBytes != 0U)
        throw std::runtime_error("cancelled partial decode lost its cursor, allocation ownership or cleanup");
    std::cout << "{\"mode\":\"" << mode << "\",\"cancelled_after_payload_allocation\":true,"
        << "\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void codecInspectionAllocationFreeCase(const std::string &mode) {
    using Codec = CanonicalPropertyValueCodec;
    const PropertyValue value(std::vector<PropertyValue>(1024U, PropertyValue(std::string(512U, 'v'))));
    std::vector<std::byte> encoded; Codec::append(encoded, value);
    size_t offset = 0U, checkpoints = 0U;
    const std::function<void()> checkpoint = [&] { ++checkpoints; };
    Codec::DecodeShape shape;
    { RejectAllocations reject; shape = Codec::inspectNext(encoded, offset, checkpoint); }
    if (shape.value_count != 1025U || shape.list_elements != 1024U || shape.string_bytes != 524288U ||
        offset != encoded.size() || checkpoints < 6U)
        throw std::runtime_error("allocation-free codec inspection changed its shape or cursor");
    struct Denied {};
    const std::function<void(const Codec::DecodeShape &)> deny = [](const auto &) { throw Denied{}; };
    bool denied = false; offset = 0U;
    {
        RejectAllocations reject;
        try { static_cast<void>(Codec::decodeNextWithAdmission(encoded, offset, deny)); }
        catch (const Denied &) { denied = true; }
    }
    if (!denied || offset != 0U) throw std::runtime_error("codec admission allocated or consumed before rejection");
    bool cancelled = false; checkpoints = 0U;
    const std::function<void()> cancel = [&] { if (++checkpoints == 2U) throw Denied{}; };
    {
        RejectAllocations reject;
        try { static_cast<void>(Codec::inspectNext(encoded, offset, cancel)); }
        catch (const Denied &) { cancelled = true; }
    }
    if (!cancelled || offset != 0U) throw std::runtime_error("codec inspection allocated or consumed before cancellation");
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_values\":" << shape.value_count
        << ",\"decoded_string_bytes\":" << shape.string_bytes << ",\"passed\":true}\n";
}

template<class Payload>
struct ObservedOverlayOwner {
    graph::storage::detail::OverlayPayloadBudget payload_budget;
    Payload payload;
    explicit ObservedOverlayOwner(std::pmr::memory_resource *memory)
        : payload_budget(memory, 2U * graph::query::execution::detail::standardContainerProxyBytes()) {}
    ObservedOverlayOwner(std::pmr::memory_resource *memory, const ObservedOverlayOwner &source)
        : payload_budget(memory, source.payload), payload(source.payload) {}
};
void overlayPayloadHeapAccountingCase(const std::string &mode) {
    auto budget = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
    PropertyValue::MapType map; map.reserve(4096U);
    map.emplace("bytes", PropertyValue(PropertyValue::Bytes(32768U, std::byte{0x42})));
    map.emplace("list", PropertyValue(std::vector<PropertyValue>(128U, PropertyValue(std::string(512U, 'p')))));
    const std::vector<LogicalGraphWriteProperty> large{{"nested", PropertyValue(std::move(map))},
        {std::string(1024U, 'k'), PropertyValue(std::string(16384U, 't'))}};
    const std::vector<LogicalGraphWriteProperty> small{{"rank", PropertyValue(7)}};
    const std::vector<std::string> labels{std::string(8192U, 'z'), std::string(4096U, 'a')};
    const std::string type(8192U, 't');
    const auto baseline = liveBytes.load(std::memory_order_relaxed);
    observedInitialLive = baseline; observedInitialCharge = budget->used(); maximumUncoveredWriteBytes = 0U;
    observedWriteBudget = budget.get();
    size_t cases = 0U;
    const auto inspect = [&] { observeWriteOwnership(liveBytes.load(std::memory_order_relaxed)); ++cases; };
    try {
        {
            ObservedOverlayOwner<LogicalGraphNode> node(budget.get());
            node.payload_budget.replaceProperties(node.payload, small); inspect();
            node.payload_budget.replaceLabels(node.payload, labels); inspect();
            node.payload_budget.replaceProperties(node.payload, large); inspect();
            {
                ObservedOverlayOwner<LogicalGraphNode> undo(budget.get(), node); inspect();
                node.payload_budget.replaceProperties(node.payload, small); inspect();
                node.payload_budget.swap(undo.payload_budget);
                node.payload.labels.swap(undo.payload.labels); node.payload.properties.swap(undo.payload.properties);
                inspect();
            }
            ObservedOverlayOwner<LogicalGraphEdge> edge(budget.get());
            edge.payload_budget.replaceType(edge.payload, type); inspect();
            edge.payload_budget.replaceProperties(edge.payload, large); inspect();
            {
                ObservedOverlayOwner<LogicalGraphEdge> undo(budget.get(), edge); inspect();
                edge.payload_budget.replaceProperties(edge.payload, small); inspect();
                edge.payload_budget.swap(undo.payload_budget);
                edge.payload.type.swap(undo.payload.type); edge.payload.properties.swap(undo.payload.properties);
                inspect();
            }
        }
        inspect();
    } catch (...) { observedWriteBudget = nullptr; throw; }
    observedWriteBudget = nullptr;
    if (maximumUncoveredWriteBytes != 0U)
        throw std::runtime_error("overlay payload had uncovered private allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
    if (budget->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
        throw std::runtime_error("overlay payload did not free data before refunding its owner");
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void nativeStageHeapAccountingCase(const std::string &mode, RelocatableStorageBackend &backend) {
    const auto ids = backend.createLogicalNodes({{{}, {}}, {{}, {}}});
    using Input = std::pair<LogicalGraphMutationBatch, std::vector<LogicalGraphPropertyPatchRequest>>;
    std::vector<Input> inputs(5U);
    const auto entity = [&](size_t index) {
        return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
            LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids.at(index)};
    };
    const auto nested = [](char fill) {
        PropertyValue::MapType map; map.reserve(4096U);
        map.emplace("bytes", PropertyValue(PropertyValue::Bytes(65536U, std::byte{0x42})));
        map.emplace("list", PropertyValue(std::vector<PropertyValue>(128U, PropertyValue(std::string(512U, fill)))));
        return PropertyValue(std::move(map));
    };
    inputs[0].first.property_replacements.push_back({entity(0U), {{"rank", PropertyValue(7)}}});
    inputs[0].first.node_label_replacements.push_back({entity(0U), {std::string(8192U, 'l'), "Short"}});
    inputs[1].second.push_back({entity(0U), {{"payload", nested('b')}, {"extra", PropertyValue(std::string(16384U, 'e'))}}, {}});
    inputs[2].second.push_back({entity(0U), {{"payload", nested('c')}}, {"extra"}});
    inputs[3].second.push_back({entity(1U), {{"rank", PropertyValue(7)}}, {}});
    inputs[4].second.push_back({entity(1U), {{"payload", nested('e')}}, {"rank"}});
    // Warm tracing/library metadata before measuring private retained storage.
    {
        auto warm = backend.beginLogicalGraphTransaction(false);
        for (const auto &input : inputs)
            (void)warm->stageMutationBatchWithPropertyPatches(input.first, input.second, 16U);
        warm->rollback();
    }
    auto transaction = backend.beginLogicalGraphTransaction(false);
    auto budget = std::dynamic_pointer_cast<graph::query::execution::MaterializedByteBudget>(
        dynamic_cast<LogicalGraphTransactionMemoryProvider &>(*transaction).retainedWriteMemory());
    if (!budget) throw std::runtime_error("native transaction lost its WRITE budget");
    const auto baseline = liveBytes.load(std::memory_order_relaxed);
    const auto initialCharge = budget->used();
    size_t maximumObserved = 0U;
    for (const auto &input : inputs) {
        LogicalGraphMutationReadControl control;
        bool observing = false;
        control.checkpoint = [&] {
            if (!observing) {
                observedInitialLive = liveBytes.load(std::memory_order_relaxed);
                observedInitialCharge = budget->used(); maximumUncoveredWriteBytes = 0U;
                observedWriteBudget = budget.get(); observing = true;
            } else observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        };
        try { (void)transaction->stageMutationBatchWithPropertyPatches(input.first, input.second, 16U, control); }
        catch (...) { observedWriteBudget = nullptr; throw; }
        observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        observedWriteBudget = nullptr;
        if (!observing || maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("native staging had uncovered live allocator requests: " + std::to_string(maximumUncoveredWriteBytes));
        const auto observed = liveBytes.load(std::memory_order_relaxed) - baseline;
        if (budget->used() - initialCharge < observed)
            throw std::runtime_error("native staged payload underestimated retained allocator requests");
        maximumObserved = (std::max)(maximumObserved, observed);
    }
    transaction->rollback();
    if (budget->used() > initialCharge || liveBytes.load(std::memory_order_relaxed) > baseline)
        throw std::runtime_error("native rollback retained its staged payload");
    transaction.reset();
    if (budget->used() != 0U) throw std::runtime_error("destroyed native transaction retained WRITE credit");
    backend.close();
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << inputs.size()
        << ",\"maximum_uncovered_private_bytes\":0,\"maximum_observed_requested_bytes\":" << maximumObserved << ",\"passed\":true}\n";
}

void scratchWriteAllocationFaultCase(const std::string &mode, const char *path, size_t fault) {
    RelocatableStorageBackendOptions options;
    options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    options.query_scratch_enabled = true;
    options.maximum_database_query_scratch_bytes = 2U * RSS_SEGMENT_SIZE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    const auto node = backend->createLogicalNodes({{{"ScratchFault"}, {{"value", PropertyValue(7)}}}}).front();
    const auto fence = backend->currentNativeLogicalGraphMutationFence();
    auto graph = backend->openLogicalGraphReadSnapshot();
    auto *provider = dynamic_cast<LogicalQueryScratchProvider *>(graph.get());
    if (!provider) throw std::runtime_error("scratch allocation fault has no provider");
    auto scratch = provider->openQueryScratchSession(2U * RSS_SEGMENT_SIZE, 2U);
    const std::vector<std::byte> warmPayload(4096U, std::byte{0x21});
    const std::vector<std::byte> payload(4096U, std::byte{0x4B});
    const auto warm = scratch->write(warmPayload);
    uint64_t handle = 0U; bool threw = false;
    faultTriggered.store(false, std::memory_order_relaxed);
    const auto before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    try { handle = scratch->write(payload); } catch (...) { threw = true; }
    const auto attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const bool triggered = faultTriggered.load(std::memory_order_relaxed);
    const bool recovery = backend->hasIndeterminateWriteOutcome();
    if (!threw) {
        if (!handle || scratch->read(handle) != payload || scratch->read(warm) != warmPayload)
            throw std::runtime_error("successful scratch receipt lost a complete fragment");
    }
    if (!recovery) {
        if (scratch->read(warm) != warmPayload) throw std::runtime_error("failed scratch write lost an earlier handle");
        scratch->finish();
    }
    scratch.reset(); graph.reset(); backend->close(); backend->open();
    if (backend->currentNativeLogicalGraphMutationFence() != fence)
        throw std::runtime_error("scratch allocation fault changed the graph publication fence");
    {
        auto current = backend->openLogicalGraphReadSnapshot();
        if (current->readNodeProperty(node, "value") != PropertyValue(7))
            throw std::runtime_error("scratch allocation fault changed durable user data");
        auto *retryProvider = dynamic_cast<LogicalQueryScratchProvider *>(current.get());
        auto retry = retryProvider->openQueryScratchSession(RSS_SEGMENT_SIZE, 1U);
        const auto id = retry->write(payload);
        if (retry->read(id) != payload) throw std::runtime_error("scratch allocation fault did not restore reusable quota");
        retry->finish();
    }
    static_cast<void>(backend->verifyIntegrity()); backend->close();
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"operation_threw\":" << (threw ? "true" : "false")
        << ",\"requires_recovery\":" << (recovery ? "true" : "false")
        << ",\"fault_triggered\":" << (triggered ? "true" : "false") << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null";
    else std::cout << fault;
    std::cout << ",\"passed\":true}\n";
}

void planCacheAllocationFaultCase(const std::string &mode, const std::filesystem::path &path, size_t fault) {
    RelocatableStorageBackendOptions options;
    options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    auto domain = backend->queryResourceDomain();
    const auto fence = backend->currentNativeLogicalGraphMutationFence();
    graph::query::QueryEngine engine(backend);
    const std::string warm = "RETURN 1 AS value";
    const std::string text = "UNWIND [1, 2, 3] AS item RETURN item + 1 AS value";
    { auto initial = engine.buildPlan(warm); }
    if (engine.getPlanCache().size() != 1U) throw std::runtime_error("plan cache warmup failed");
    bool threw = false;
    std::optional<graph::query::QueryPlan> plan;
    faultTriggered.store(false, std::memory_order_relaxed);
    const auto before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    try { plan.emplace(engine.buildPlan(text)); }
    catch (const std::exception &) { threw = true; }
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const auto attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    const bool triggered = faultTriggered.load(std::memory_order_relaxed);
    const auto verify = [](const graph::query::QueryResult &result) {
        if (result.rowCount() != 3U) throw std::runtime_error("plan cache fault returned incomplete rows");
        for (size_t index = 0U; index < 3U; ++index)
            if (result.getRows()[index].at("value").asPrimitive() != PropertyValue(static_cast<int64_t>(index + 2U)))
                throw std::runtime_error("plan cache fault changed the result values");
    };
    if (threw && engine.getPlanCache().size() != 1U)
        throw std::runtime_error("failed plan cache insertion changed prior entries");
    if (plan) verify(engine.executePlan(std::move(*plan), {}));
    plan.reset();
    if (domain->usage().classes[static_cast<size_t>(graph::query::execution::detail::ResourceClass::RCK_QUERY)] != 0U)
        throw std::runtime_error("plan cache fault retained compilation query credit");
    const auto old = engine.execute(warm);
    if (old.rowCount() != 1U || old.getRows()[0].at("value").asPrimitive() != PropertyValue(int64_t{1}))
        throw std::runtime_error("plan cache fault damaged its previous query");
    verify(engine.execute(text));
    if (backend->hasIndeterminateWriteOutcome() || backend->currentNativeLogicalGraphMutationFence() != fence)
        throw std::runtime_error("plan cache allocation fault published or fenced a write");
    engine.resetEphemeralState();
    static_cast<void>(backend->verifyIntegrity()); backend->close();
    // The separately retained warm result remains a valid charged owner here.
    if (domain->usage().classes[static_cast<size_t>(graph::query::execution::detail::ResourceClass::RCK_CACHE)] != 0U)
        throw std::runtime_error("plan cache cleanup retained CACHE credit");
    if (domain->usage().total != old.retainedResourceBytes())
        throw std::runtime_error("plan cache cleanup retained unexpected resource credit");
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"operation_threw\":" << (threw ? "true" : "false")
        << ",\"fault_triggered\":" << (triggered ? "true" : "false") << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null"; else std::cout << fault;
    std::cout << ",\"passed\":true}\n";
}

void distinctAllocationFaultCase(const std::string &mode, const char *path, size_t fault) {
    using namespace graph::query;
    using namespace graph::query::execution::detail;
    RelocatableStorageBackendOptions options;
    options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    options.query_scratch_enabled = true;
    options.maximum_database_query_scratch_bytes = 8U * RSS_SEGMENT_SIZE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    const auto node = backend->createLogicalNodes({{{"DistinctFault"}, {{"value", PropertyValue(7)}}}}).front();
    const auto fence = backend->currentNativeLogicalGraphMutationFence();
    LogicalReadExecutionOptions reads; reads.bounds.maximum_materialized_bytes = 8U * 1024U * 1024U;
    QueryContext context;
    context.resource_account = std::make_shared<ResourceAccount>(backend->queryResourceDomain(), ResourceClass::RCK_QUERY,
        reads.bounds.maximum_materialized_bytes);
    const std::string alias(2048U, 'a');
    const std::string text = "UNWIND range(1,800) AS x WITH DISTINCT x % 400 AS `" + alias +
        "` RETURN collect(`" + alias + "`) AS values";
    const auto validate = [](const QueryResult &result) {
        if (result.rowCount() != 1U) throw std::runtime_error("DISTINCT fault returned an incomplete output row");
        const auto &values = result.getRows()[0].at("values").asPrimitive().getList();
        if (values.size() != 400U) throw std::runtime_error("DISTINCT fault returned an incomplete collection");
        for (size_t i = 0U; i < values.size(); ++i)
            if (values[i] != PropertyValue(static_cast<int64_t>((i + 1U) % 400U)))
                throw std::runtime_error("DISTINCT fault changed equality or first-occurrence order");
    };
    bool threw = false, recovery = false, triggered = false;
    size_t attempts = 0U;
    {
        QueryEngine engine(std::static_pointer_cast<LogicalGraphReadBackend>(backend), reads);
        validate(engine.execute(text, context));
        faultTriggered.store(false, std::memory_order_relaxed);
        const auto before = allocationAttempts.load(std::memory_order_relaxed);
        faultRemaining.store(fault, std::memory_order_relaxed);
        std::optional<QueryResult> result;
        try { result.emplace(engine.execute(text, context)); }
        catch (const std::exception &) { threw = true; }
        faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
        attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
        // Validate outside the exception capture: an incomplete successful
        // result must fail the worker rather than masquerade as a rejected query.
        if (result) validate(*result);
        result.reset();
        triggered = faultTriggered.load(std::memory_order_relaxed);
        recovery = backend->hasIndeterminateWriteOutcome();
        if (context.resource_account->used() != 0U)
            throw std::runtime_error("DISTINCT fault retained a query or scratch allocation charge");
    }
    backend->close(); backend->open();
    if (backend->currentNativeLogicalGraphMutationFence() != fence)
        throw std::runtime_error("DISTINCT fault changed the graph publication fence");
    {
        auto graph = backend->openLogicalGraphReadSnapshot();
        if (graph->readNodeProperty(node, "value") != PropertyValue(7))
            throw std::runtime_error("DISTINCT fault changed durable user data");
    }
    {
        QueryEngine retry(std::static_pointer_cast<LogicalGraphReadBackend>(backend), reads);
        validate(retry.execute(text, context));
    }
    if (context.resource_account->used() != 0U)
        throw std::runtime_error("DISTINCT retry leaked query memory");
    static_cast<void>(backend->verifyIntegrity()); backend->close();
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"operation_threw\":" << (threw ? "true" : "false")
        << ",\"requires_recovery\":" << (recovery ? "true" : "false")
        << ",\"fault_triggered\":" << (triggered ? "true" : "false") << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null";
    else std::cout << fault;
    std::cout << ",\"passed\":true}\n";
}

void leaseAllocationFaultCase(const std::string &mode, const char *path, size_t fault) {
    RelocatableStorageBackendOptions options;
    options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    auto backend = RelocatableStorageBackend::initializeNew(options);
    backend->open(); backend->close(); backend.reset();
    auto stream = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!stream->is_open()) throw std::runtime_error("lease fault could not open metadata file");
    auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    auto store = RelocatableMetadataStore::open(io);
    const auto warm = store->acquireNativeGraphLogicalIds(1U, 1U, 2U);
    if (warm.node_ranges.size() != 1U || warm.edge_ranges.size() != 1U)
        throw std::runtime_error("lease fault warm range count changed");
    Superblock beforeRoot;
    { auto pin = store->openSnapshot(); beforeRoot = pin.superblock(); }
    const bool direct = mode == "lease-reserve-allocation-fault";
    const bool fresh = mode == "lease-fresh-allocation-fault";
    NativeGraphLogicalIdReservation expected;
    if (direct) {
        expected.node_ranges = {{beforeRoot.next_logical_node_id, 1U}};
        expected.edge_ranges = {{beforeRoot.next_logical_edge_id, 1U}};
    } else {
        expected.node_ranges = {{warm.node_ranges.front().first_id + 1U, 1U}};
        expected.edge_ranges = {{warm.edge_ranges.front().first_id + 1U, 1U}};
        if (fresh) {
            expected.node_ranges.push_back({beforeRoot.next_logical_node_id, 2U});
            expected.edge_ranges.push_back({beforeRoot.next_logical_edge_id, 2U});
        }
    }
    NativeGraphLogicalIdReservation result;
    result.node_ranges.reserve(2U); result.edge_ranges.reserve(2U);
    bool threw = false;
    faultTriggered.store(false, std::memory_order_relaxed);
    const auto before = allocationAttempts.load(std::memory_order_relaxed);
    faultRemaining.store(fault, std::memory_order_relaxed);
    try {
        if (direct) {
            const auto lease = store->reserveNativeGraphLogicalIds(1U, 1U);
            result.node_ranges.push_back(lease.nodes);
            result.edge_ranges.push_back(lease.edges);
        } else {
            result = store->acquireNativeGraphLogicalIds(fresh ? 3U : 1U, fresh ? 3U : 1U, 8U);
        }
    } catch (const std::exception &) { threw = true; }
    faultRemaining.store((std::numeric_limits<size_t>::max)(), std::memory_order_relaxed);
    const auto attempts = allocationAttempts.load(std::memory_order_relaxed) - before;
    const bool triggered = faultTriggered.load(std::memory_order_relaxed);
    if (!triggered && threw) throw std::runtime_error("normal logical-ID lease allocation probe failed");
    if (!store->claimNativeGraphLogicalIdReservation(warm))
        throw std::runtime_error("failed acquisition changed an unrelated live lease capability");
    if (threw) {
        const auto rejectPhantoms = [&](const auto &ranges, bool node) {
            for (const auto &range : ranges) {
                NativeGraphLogicalIdReservation one;
                (node ? one.node_ranges : one.edge_ranges).push_back(range);
                if (store->claimNativeGraphLogicalIdReservation(one))
                    throw std::runtime_error("failed logical-ID acquisition leaked a claimable partial capability");
            }
        };
        rejectPhantoms(expected.node_ranges, true);
        rejectPhantoms(expected.edge_ranges, false);
    } else {
        if (result != expected || !store->claimNativeGraphLogicalIdReservation(result) ||
            store->claimNativeGraphLogicalIdReservation(result))
            throw std::runtime_error("successful logical-ID acquisition did not return one complete one-shot capability");
    }
    store.reset(); io.reset(); stream->close(); stream.reset();
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend reopened(options); reopened.open();
    (void)reopened.verifyIntegrity(); reopened.close();
    std::cout << "{\"mode\":\"" << mode << "\",\"allocation_attempts\":" << attempts
        << ",\"fault_triggered\":" << (triggered ? "true" : "false")
        << ",\"operation_threw\":" << (threw ? "true" : "false") << ",\"selected_fault\":";
    if (fault == (std::numeric_limits<size_t>::max)()) std::cout << "null";
    else std::cout << fault;
    std::cout << ",\"passed\":true}\n";
}

void scratchCrashCase(const std::string &mode, const char *path) {
    RelocatableStorageBackendOptions options;
    options.data_path = path;
    options.query_scratch_enabled = true;
    options.maximum_database_query_scratch_bytes = RSS_SEGMENT_SIZE;
    if (mode == "crash-query-scratch" || mode == "crash-existing-query-scratch") {
        const bool existing = mode == "crash-existing-query-scratch";
        options.open_mode = existing ? OpenMode::OPEN_EXISTING_FILE : OpenMode::OPEN_CREATE_NEW_FILE;
        auto backend = existing ? std::make_unique<RelocatableStorageBackend>(options)
                                : RelocatableStorageBackend::initializeNew(options);
        backend->open();
        if (!existing) {
            const auto id = backend->createLogicalNodes({{{"CrashSeed"}, {{"value", PropertyValue(int64_t{7})}}}}).front();
            if (id != 1U) throw std::runtime_error("scratch crash seed identity changed");
        } else {
            auto snapshot = backend->openLogicalGraphReadSnapshot();
            if (snapshot->readNodeProperty(1U, "value") != PropertyValue(int64_t{7}))
                throw std::runtime_error("repeated scratch crash changed user graph data");
        }
        auto scratch = backend->openNativeQueryScratchSession(RSS_SEGMENT_SIZE, 1U);
        if (!scratch) throw std::runtime_error("scratch crash could not open a private session");
        const std::vector<std::byte> bytes(4096U, std::byte{0x3A});
        const auto handle = scratch->write(bytes);
        if (scratch->read(handle) != bytes) throw std::runtime_error("scratch crash payload changed");
        // Deliberately skip every destructor and normal scratch release.
        std::_Exit(87);
    }
    const bool readonly = mode == "inspect-query-scratch-crash";
    options.open_mode = readonly ? OpenMode::OPEN_READ_ONLY_EXISTING_FILE : OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open(); backend.resetIoStats();
    {
        auto snapshot = backend.openLogicalGraphReadSnapshot();
        if (snapshot->readNodeProperty(1U, "value") != PropertyValue(int64_t{7}))
            throw std::runtime_error("scratch crash recovery changed user graph data");
    }
    if (readonly) {
        bool rejected = false;
        try { (void)backend.verifyIntegrity(); } catch (const FormatException &) { rejected = true; }
        if (!rejected) throw std::runtime_error("readonly integrity ignored orphaned scratch");
        if (backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE, 1U))
            throw std::runtime_error("physical readonly recovery permitted new scratch");
        if (backend.ioStats().data.write_bytes != 0U)
            throw std::runtime_error("readonly orphan inspection wrote database bytes");
    } else {
        (void)backend.verifyIntegrity();
        // The old one-segment per-database scratch quota must be reusable now.
        auto scratch = backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE, 1U);
        if (!scratch) throw std::runtime_error("recovered scratch quota was not reusable");
        const std::vector<std::byte> bytes(4096U, std::byte{0x6B});
        const auto handle = scratch->write(bytes);
        if (scratch->read(handle) != bytes) throw std::runtime_error("recovered scratch payload changed");
        scratch->finish(); (void)backend.verifyIntegrity();
    }
    backend.close();
    std::cout << "{\"mode\":\"" << mode << "\",\"readonly\":" << (readonly ? "true" : "false")
        << ",\"passed\":true}\n";
}
}
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) return 2;
    try {
        const std::string mode = argv[1];
        if (mode == "crash-query-scratch" || mode == "crash-existing-query-scratch" || mode == "inspect-query-scratch-crash" || mode == "recover-query-scratch-crash") {
            scratchCrashCase(mode, argv[2]);
            return 0;
        }
        if (mode == "source-materialization-allocation-fault") {
            const size_t fault = argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : (std::numeric_limits<size_t>::max)();
            sourceMaterializationReadFaultCase(mode, argv[2], fault); return 0;
        }
        if (mode == "source-materialization-copy-accounting") {
            sourceMaterializationCopyCase(mode, argv[2]); return 0;
        }
        if (mode == "schema-cache-allocation-fault") {
            const size_t fault = argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : (std::numeric_limits<size_t>::max)();
            schemaCacheAllocationFaultCase(mode, fault); return 0;
        }
        if (mode == "owned-constraint-definition-heap-accounting") { ownedSchemaDefinitionHeapAccountingCase<ConstraintDefinition, ConstraintDefinitionCodec>(mode); return 0; }
        if (mode == "owned-index-definition-heap-accounting") { ownedSchemaDefinitionHeapAccountingCase<IndexDefinition, IndexDefinitionCodec>(mode); return 0; }
        if (mode == "query-context-account-validation") { queryContextAccountValidationCase(mode, argv[2]); return 0; }
        if (mode == "result-column-copy-admission") { resultColumnCopyAdmissionCase(mode); return 0; }
        if (mode == "owned-plan-heap-accounting") { ownedPlanHeapAccountingCase(mode); return 0; }
        if (mode == "owned-string-containers-heap-accounting") { ownedStringContainersHeapAccountingCase(mode); return 0; }
        if (mode == "owned-property-heap-accounting") { ownedPropertyHeapAccountingCase(mode); return 0; }
        if (mode == "owned-expression-heap-accounting") { ownedExpressionHeapAccountingCase(mode); return 0; }
        if (mode == "owned-std-string-heap-accounting") { ownedStdStringHeapAccountingCase(mode); return 0; }
        if (mode == "codec-scoped-partial-cancellation") {
            scopedDecodePartialCancellationCase(mode); return 0;
        }
        if (mode == "codec-admitted-heap-accounting" || mode == "codec-scoped-heap-accounting" || mode == "guarded-copy-heap-accounting") {
            admittedCodecHeapAccountingCase(mode); return 0;
        }
        if (mode == "codec-inspection-allocation-free") {
            codecInspectionAllocationFreeCase(mode); return 0;
        }
        if (mode == "overlay-payload-heap-accounting") {
            overlayPayloadHeapAccountingCase(mode); return 0;
        }
        if (mode == "property-heap-accounting") {
            propertyHeapAccountingCase(mode);
            return 0;
        }
        if (mode == "plan-cache-allocation-fault") {
            const size_t fault = argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : (std::numeric_limits<size_t>::max)();
            planCacheAllocationFaultCase(mode, argv[2], fault); return 0;
        }
        if (mode == "distinct-allocation-fault") {
            const size_t fault = argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : (std::numeric_limits<size_t>::max)();
            distinctAllocationFaultCase(mode, argv[2], fault);
            return 0;
        }
        if (mode == "scratch-write-allocation-fault") {
            const size_t fault = argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : (std::numeric_limits<size_t>::max)();
            scratchWriteAllocationFaultCase(mode, argv[2], fault);
            return 0;
        }
        if (mode == "lease-reserve-allocation-fault" || mode == "lease-cached-allocation-fault" ||
            mode == "lease-fresh-allocation-fault") {
            const size_t fault = argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : (std::numeric_limits<size_t>::max)();
            leaseAllocationFaultCase(mode, argv[2], fault);
            return 0;
        }
        const bool allocationFault = mode == "stage-allocation-fault" || mode == "commit-allocation-fault" || mode == "overlay-stage-allocation-fault" || mode == "creation-stage-allocation-fault" || mode == "overlay-creation-stage-allocation-fault" || mode == "mixed-stage-allocation-fault" || mode == "normalized-patch-allocation-fault";
        if (!allocationFault && mode != "native-stage-heap-accounting" && mode != "rollback-oom" && mode != "rollback-many-oom" && mode != "rollback-retention" && mode != "commit-retention") return 2;
        RelocatableStorageBackendOptions options;
        options.data_path = argv[2];
        options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
        auto backend = RelocatableStorageBackend::initializeNew(options);
        backend->open();
        if (mode == "native-stage-heap-accounting") {
            nativeStageHeapAccountingCase(mode, *backend); return 0;
        }
        if (allocationFault) {
            const size_t fault = argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : (std::numeric_limits<size_t>::max)();
            if (mode == "normalized-patch-allocation-fault") normalizedPatchAllocationFaultCase(mode, *backend, fault);
            else if (mode == "mixed-stage-allocation-fault") mixedStatementAllocationFaultCase(mode, *backend, fault);
            else if (mode == "creation-stage-allocation-fault" || mode == "overlay-creation-stage-allocation-fault") creationAllocationFaultCase(mode, *backend, fault);
            else allocationFaultCase(mode, *backend, fault);
            return 0;
        }
        auto transaction = backend->beginLogicalGraphTransaction(false);
        size_t retained = 0U;
        if (mode == "commit-retention") {
            const auto id = stageNodes(*transaction, 1U, 1024U * 1024U);
            const auto result = transaction->commit();
            if (transaction->state() != LogicalGraphTransactionState::LGTTS_COMMITTED || result.created_node_ids != std::vector<uint64_t>{id}) return 3;
            retained = destroyClosed(transaction);
            auto snapshot = backend->openLogicalGraphReadSnapshot();
            if (snapshot->readNodeProperty(id, "payload") != PropertyValue(std::string(1024U * 1024U, 'q'))) return 4;
        } else {
            const size_t statements = mode == "rollback-many-oom" ? 40U : 1U;
            uint64_t abandoned = 0U;
            for (size_t statement = 0U; statement < statements; ++statement)
                abandoned = stageNodes(*transaction, mode == "rollback-retention" ? 8192U : 1U);
            {
                RejectAllocations rejection;
                transaction->rollback();
            }
            if (transaction->state() != LogicalGraphTransactionState::LGTTS_ROLLED_BACK) return 5;
            retained = destroyClosed(transaction);
            auto next = backend->beginLogicalGraphTransaction(false);
            const uint64_t nextId = stageNodes(*next, 1U);
            if (nextId <= abandoned) return 6;
            const auto result = next->commit();
            if (result.created_node_ids != std::vector<uint64_t>{nextId}) return 7;
            next.reset();
            auto snapshot = backend->openLogicalGraphReadSnapshot();
            if (snapshot->readNodeShaped(nextId, {}).node_id != nextId) return 8;
        }
        static_cast<void>(backend->verifyIntegrity());
        backend->close();
        backend->open();
        static_cast<void>(backend->verifyIntegrity());
        backend->close();
        std::cout << "{\"mode\":\"" << mode << "\",\"closed_object_bytes\":" << retained << ",\"passed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        denyAllocations.store(false, std::memory_order_relaxed);
        std::cerr << error.what() << '\n';
        return 10;
    }
}
