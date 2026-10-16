/**
 * @brief Private admission of procedure row/cache payloads across borrowed-row calls.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <type_traits>
#include "graph/utils/OwnedStdString.hpp"
#include <optional>
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include <utility>
#include "graph/core/PropertyHeapRequestBytes.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/UnmanagedAllocationResource.hpp"

namespace graph::query::execution::detail {
[[nodiscard]] utils::OwnedStdString makeOwnedProcedureDistinctRowKey(
    const LogicalPlanReadRow &row, const std::vector<std::string> &columns);

// These rows never escape LogicalPlanProcedureRuntime::execute. Its outer owned
// allocation scope retains the authority, so per-row credits need only two words.
class ProcedurePayloadCredit final {
public:
    explicit ProcedurePayloadCredit(size_t bytes = 0U)
        : resource_(dynamic_cast<utils::UnmanagedAllocationResource *>(utils::OwnedAllocationScope::owner().get())) {
        set(bytes);
    }
    ProcedurePayloadCredit(utils::UnmanagedAllocationResource *resource, size_t bytes)
        : resource_(resource) { set(bytes); }
    ~ProcedurePayloadCredit() { if (resource_) resource_->releaseUnmanaged(bytes_); }
    ProcedurePayloadCredit(const ProcedurePayloadCredit &) = delete;
    ProcedurePayloadCredit &operator=(const ProcedurePayloadCredit &) = delete;
    void set(size_t bytes) {
        if (!resource_) return;
        if (bytes > bytes_) resource_->reserveUnmanaged(bytes - bytes_);
        else if (bytes < bytes_) resource_->releaseUnmanaged(bytes_ - bytes);
        bytes_ = bytes;
    }
    void add(size_t bytes) { if (resource_) set(utils::detail::addStandardHeapBytes(bytes_, bytes)); }
    [[nodiscard]] ResourceLease detachCharged(size_t bytes) {
        auto *budget = dynamic_cast<MaterializedByteBudget *>(resource_);
        if (!budget || bytes > bytes_) throw std::logic_error("procedure output lacks admitted payload ownership: requested=" + std::to_string(bytes) + ", admitted=" + std::to_string(bytes_));
        auto lease = budget->detachCharged(bytes); bytes_ -= bytes; return lease;
    }
    void adoptCharged(ResourceLease &&lease) noexcept {
        auto *budget = dynamic_cast<MaterializedByteBudget *>(resource_);
        if (!budget || lease.bytes() > (std::numeric_limits<size_t>::max)() - bytes_) std::terminate();
        bytes_ += lease.bytes(); budget->adoptCharged(std::move(lease));
    }
    void swap(ProcedurePayloadCredit &other) noexcept {
        std::swap(resource_, other.resource_); std::swap(bytes_, other.bytes_);
    }
    [[nodiscard]] auto resource() const noexcept { return resource_; }
    [[nodiscard]] size_t bytes() const noexcept { return bytes_; }
private:
    utils::UnmanagedAllocationResource *resource_;
    size_t bytes_ = 0U;
};

// Private output publication transfers an existing allocation fee. The public
// owning result retains its account after this synchronous query scope exits.
struct ProcedureResultHandoff final {
    static void append(QueryResult &result, QueryResult::Row &&row, ProcedurePayloadCredit &credit) {
        if (!result.resource_charge_) { result.addRow(std::move(row)); return; }
        auto *budget = dynamic_cast<MaterializedByteBudget *>(credit.resource());
        if (!budget) { result.addRow(std::move(row)); return; }
        result.reserveRows(result.rows_.size() + 1U);
        auto lease = credit.detachCharged(graph::query::detail::resultRowHeapBytes(row));
        if (!result.resource_charge_->sameAccount(lease)) {
            credit.adoptCharged(std::move(lease));
            throw std::logic_error("procedure result crosses query allocation ownership");
        }
        try { result.rows_.push_back(std::move(row)); }
        catch (...) { credit.adoptCharged(std::move(lease)); throw; }
        result.resource_charge_->absorb(std::move(lease));
    }
};

// QueryContext itself outlives synchronous execution, including all nested
// decode/copy checkpoints installed below.
class ProcedureQueryCheckpointScope final {
public:
    explicit ProcedureQueryCheckpointScope(const QueryContext &context) noexcept
        : previous_(active_) { active_ = &context; }
    ~ProcedureQueryCheckpointScope() { active_ = previous_; }
    static void check() { if (active_) active_->checkGuardNow(); }
private:
    const QueryContext *previous_;
    inline static thread_local const QueryContext *active_ = nullptr;
};

class ProcedurePayloadAdmissionScope final {
public:
    explicit ProcedurePayloadAdmissionScope(ProcedurePayloadCredit &credit) noexcept
        : credit_(credit), previous_(active_), previous_decode_(property_decode::AllocationScope::current()) {
        if (credit.resource()) decode_.emplace(this,
            [](void *context, size_t bytes) {
                auto &scope = *static_cast<ProcedurePayloadAdmissionScope *>(context);
                scope.check(); scope.credit_.add(bytes);
            }, [](void *context) { static_cast<ProcedurePayloadAdmissionScope *>(context)->check(); }, true);
        active_ = this;
    }
    ~ProcedurePayloadAdmissionScope() { active_ = previous_; }
    ProcedurePayloadAdmissionScope(const ProcedurePayloadAdmissionScope &) = delete;
    ProcedurePayloadAdmissionScope &operator=(const ProcedurePayloadAdmissionScope &) = delete;
    [[nodiscard]] static bool active() noexcept { return active_ && active_->credit_.resource(); }
    [[nodiscard]] static ProcedurePayloadCredit *credit() noexcept { return active() ? &active_->credit_ : nullptr; }
    static void admit(size_t bytes) { if (active()) { active_->check(); active_->credit_.add(bytes); } }
private:
    void check() const {
        ProcedureQueryCheckpointScope::check();
        if (previous_decode_) previous_decode_->checkpoint();
    }
    ProcedurePayloadCredit &credit_;
    ProcedurePayloadAdmissionScope *previous_;
    property_decode::AllocationScope *previous_decode_;
    std::optional<property_decode::AllocationScope> decode_;
    inline static thread_local ProcedurePayloadAdmissionScope *active_ = nullptr;
};

// This sidecar is deliberately opt-in: ordinary plan execution keeps its
// existing local-budget behavior. Only a synchronous procedure row receives
// released manual reservations; freed PMR buffers are always refunded normally.
class ProcedureBudgetHandoff final {
public:
    explicit ProcedureBudgetHandoff(MaterializedByteBudget &source) noexcept {
        auto *credit = ProcedurePayloadAdmissionScope::credit();
        auto *target = credit ? dynamic_cast<MaterializedByteBudget *>(credit->resource()) : nullptr;
        if (target && source.canTransferTo(*target)) source.setReservationReleaseSink(credit,
            [](void *context, ResourceLease &&lease) noexcept {
                static_cast<ProcedurePayloadCredit *>(context)->adoptCharged(std::move(lease));
            });
    }
};

template<class Map>
[[nodiscard]] size_t procedureEmptyMapHeapUpper() {
#if defined(_MSVC_STL_VERSION)
    using namespace utils::detail;
    using Entry = typename Map::value_type;
    constexpr auto node = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
    return addStandardHeapBytes(2U * standardContainerProxyBytes(),
        addStandardHeapBytes(standardAllocatorRequestBytes(node, alignof(Entry)),
            standardAllocatorRequestBytes(16U * sizeof(void *), alignof(void *))));
#else
    return 0U;
#endif
}

template<bool Copy, class T>
[[nodiscard]] size_t procedurePayloadHeapUpper(const T &value) {
    using Type = std::remove_cvref_t<T>;
    using namespace utils::detail;
    const auto add = [](size_t &total, size_t part) { total = addStandardHeapBytes(total, part); };
    if constexpr (std::is_same_v<Type, PropertyValue>) {
        return Copy ? property_heap::copyRequestUpper(value) : property_heap::retainedRequestUpper(value);
    } else if constexpr (std::is_same_v<Type, std::string>) {
        return Copy ? standardFreshStringHeapUpper(value.size()) : dynamicStringBufferBytes(value);
    } else if constexpr (Copy && std::is_convertible_v<const T &, std::string_view>) {
        return standardFreshStringHeapUpper(std::string_view(value).size());
    } else if constexpr (std::is_same_v<Type, LogicalPlanReadValue>) {
        return std::visit([](const auto &item) { return procedurePayloadHeapUpper<Copy>(item); }, value);
    } else if constexpr (std::is_same_v<Type, storage::LogicalGraphPropertyEntry>) {
        return addStandardHeapBytes(procedurePayloadHeapUpper<Copy>(value.key), procedurePayloadHeapUpper<Copy>(value.value));
    } else if constexpr (std::is_same_v<Type, storage::LogicalGraphNode>) {
        return addStandardHeapBytes(procedurePayloadHeapUpper<Copy>(value.labels), procedurePayloadHeapUpper<Copy>(value.properties));
    } else if constexpr (std::is_same_v<Type, storage::LogicalGraphEdge>) {
        return addStandardHeapBytes(procedurePayloadHeapUpper<Copy>(value.type), procedurePayloadHeapUpper<Copy>(value.properties));
    } else if constexpr (std::is_same_v<Type, LogicalPlanReadPath>) {
        size_t total = procedurePayloadHeapUpper<Copy>(value.nodes);
        add(total, procedurePayloadHeapUpper<Copy>(value.edges));
        add(total, procedurePayloadHeapUpper<Copy>(value.node_refs));
        add(total, procedurePayloadHeapUpper<Copy>(value.edge_refs));
        return total;
    } else if constexpr (std::is_same_v<Type, LogicalPlanReadRow>) {
        size_t total = procedurePayloadHeapUpper<Copy>(value.values);
        add(total, procedurePayloadHeapUpper<Copy>(value.property_cache));
        add(total, procedurePayloadHeapUpper<Copy>(value.entity_metadata_cache));
        add(total, procedurePayloadHeapUpper<Copy>(value.complete_entity_property_cache));
        add(total, procedurePayloadHeapUpper<Copy>(value.path_cache));
        return total;
    } else if constexpr (requires { typename Type::mapped_type; }) {
        size_t total = 0U;
#if !defined(_MSVC_STL_VERSION)
        if (!value.empty() || value.bucket_count() != 1U)
#endif
            total = standardUnorderedMapStructuralHeapBytes(value);
        for (const auto &[key, item] : value) {
            add(total, procedurePayloadHeapUpper<Copy>(key));
            add(total, procedurePayloadHeapUpper<Copy>(item));
        }
        return total;
    } else if constexpr (requires { typename Type::key_type; }) {
        // A set has the same linked-node/bucket policy, with a value-only node.
        using Entry = typename Type::value_type;
        constexpr size_t node = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
#if defined(_MSVC_STL_VERSION)
        size_t total = addStandardHeapBytes(2U * standardContainerProxyBytes(),
            addStandardHeapBytes(standardAllocatorRequestBytes((value.size() + 1U) * node, alignof(Entry)),
                standardAllocatorRequestBytes(value.bucket_count() * 2U * sizeof(void *), alignof(void *))));
#else
        size_t total = value.empty() && value.bucket_count() == 1U ? 0U :
            addStandardHeapBytes(standardAllocatorRequestBytes(value.size() * node, alignof(Entry)),
                standardAllocatorRequestBytes(value.bucket_count() * sizeof(void *), alignof(void *)));
#endif
        for (const auto &item : value) add(total, procedurePayloadHeapUpper<Copy>(item));
        return total;
    } else if constexpr (requires { value.capacity(); value.data(); typename Type::value_type; }) {
        using Element = typename Type::value_type;
        const size_t count = Copy ? value.size() : value.capacity();
        if (count > (std::numeric_limits<size_t>::max)() / sizeof(Element)) throw std::length_error("procedure vector heap overflowed");
        size_t total = addStandardHeapBytes(standardContainerProxyBytes(), standardAllocatorRequestBytes(count * sizeof(Element), alignof(Element)));
        for (const auto &item : value) add(total, procedurePayloadHeapUpper<Copy>(item));
        return total;
    } else if constexpr (requires { value.values; value.complete; }) {
        return procedurePayloadHeapUpper<Copy>(value.values);
    } else {
        static_assert(std::is_trivially_copyable_v<Type>, "procedure payload requires an explicit heap policy");
        return 0U;
    }
}


// Reserve bucket growth separately from nodes; charging every existing node on
// each insertion would accumulate quadratic unused grants in a wide cache.
template<class Map>
void procedureMapReserve(Map &map, size_t count) {
    if (!count) return;
    if (ProcedurePayloadAdmissionScope::active()) {
        using namespace utils::detail;
        if (count > ((std::numeric_limits<size_t>::max)() - 1U) / 2U) throw std::length_error("procedure hash capacity overflowed");
#if defined(_MSVC_STL_VERSION)
        const size_t buckets = (std::max)(size_t{8U}, 2U * count + 1U);
        const size_t width = 2U * sizeof(void *);
#else
        const size_t buckets = 2U * count + 1U;
        const size_t width = sizeof(void *);
#endif
        if (buckets > (std::numeric_limits<size_t>::max)() / width) throw std::length_error("procedure hash bytes overflowed");
        ProcedurePayloadAdmissionScope::admit(standardAllocatorRequestBytes(buckets * width, alignof(void *)));
    }
    map.reserve(count);
}

template<class Map, class Key>
void procedureAdmitMapNode(Map &map, const Key &key) {
    if (!ProcedurePayloadAdmissionScope::active()) return;
    using namespace utils::detail;
    using Entry = typename Map::value_type;
    constexpr size_t node = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
    ProcedurePayloadAdmissionScope::admit(addStandardHeapBytes(standardAllocatorRequestBytes(node, alignof(Entry)),
        ([](const auto &input) {
            if constexpr (std::is_same_v<typename Map::key_type, std::string_view>) return size_t{0U};
            else return procedurePayloadHeapUpper<true>(input);
        }(key))));
    const size_t count = addStandardHeapBytes(map.size(), 1U);
    if (map.empty() || count > map.bucket_count() * map.max_load_factor()) {
        const size_t doubled = addStandardHeapBytes(map.size(), map.size());
        procedureMapReserve(map, (std::max)(count, doubled));
    }
}

template<class Map, class Key, class Value>
auto procedureMapEmplace(Map &map, Key &&key, Value &&value) {
    if constexpr (std::is_same_v<typename Map::key_type, std::string> && !std::is_same_v<std::remove_cvref_t<Key>, std::string>)
        ProcedurePayloadAdmissionScope::admit(procedurePayloadHeapUpper<true>(key));
    if (ProcedurePayloadAdmissionScope::active() && map.find(key) == map.end()) {
        procedureAdmitMapNode(map, key);
        ProcedurePayloadAdmissionScope::admit(procedurePayloadHeapUpper<true>(value));
    }
    return map.emplace(std::forward<Key>(key), std::forward<Value>(value));
}

template<class Map, class Key>
auto procedureMapTryEmplace(Map &map, Key &&key) {
    if constexpr (std::is_same_v<typename Map::key_type, std::string> && !std::is_same_v<std::remove_cvref_t<Key>, std::string>)
        ProcedurePayloadAdmissionScope::admit(procedurePayloadHeapUpper<true>(key));
    if (ProcedurePayloadAdmissionScope::active() && map.find(key) == map.end()) {
        procedureAdmitMapNode(map, key);
        using Value = typename Map::mapped_type;
        if constexpr (requires { typename Value::mapped_type; })
            ProcedurePayloadAdmissionScope::admit(procedureEmptyMapHeapUpper<Value>());
        else if constexpr (requires { std::declval<Value>().values; std::declval<Value>().complete; })
            ProcedurePayloadAdmissionScope::admit(procedureEmptyMapHeapUpper<std::remove_cvref_t<decltype(std::declval<Value>().values)>>());
    }
    return map.try_emplace(std::forward<Key>(key));
}

template<class Map, class Key, class Value>
auto procedureMapInsertOrAssign(Map &map, Key &&key, Value &&value) {
    if (ProcedurePayloadAdmissionScope::active()) {
        if (map.find(key) == map.end()) procedureAdmitMapNode(map, key);
        ProcedurePayloadAdmissionScope::admit(procedurePayloadHeapUpper<true>(value));
    }
    return map.insert_or_assign(std::forward<Key>(key), std::forward<Value>(value));
}

template<class Set, class Key>
auto procedureSetInsert(Set &set, Key &&key) {
    if (ProcedurePayloadAdmissionScope::active() && set.find(key) == set.end()) procedureAdmitMapNode(set, key);
    return set.insert(std::forward<Key>(key));
}

template<class Vector>
void procedureVectorReserve(Vector &vector, size_t count) {
    if constexpr (requires { typename Vector::key_type; }) {
        procedureMapReserve(vector, count);
    } else {
        if (ProcedurePayloadAdmissionScope::active() && count > vector.capacity()) {
            using T = typename Vector::value_type;
            if (count > (std::numeric_limits<size_t>::max)() / sizeof(T)) throw std::length_error("procedure vector bytes overflowed");
            ProcedurePayloadAdmissionScope::admit(utils::detail::addStandardHeapBytes(
                utils::detail::standardContainerProxyBytes(), utils::detail::standardAllocatorRequestBytes(count * sizeof(T), alignof(T))));
        }
        vector.reserve(count);
    }
}

} // namespace graph::query::execution::detail
