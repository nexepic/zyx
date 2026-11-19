#pragma once
#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
namespace graph::query::execution::detail {
// The producer initializes storage directly: no extra variant/string move or
// deep copy is needed between evaluation and the scalar consumer.
class ScopedScalarReadValue final {
public:
    explicit ScopedScalarReadValue(const PropertyValue &borrowed) : value_(&borrowed), borrowed_(true) {}
    template<class Producer> requires std::invocable<Producer> &&
        std::same_as<std::invoke_result_t<Producer>, LogicalPlanReadValue>
    explicit ScopedScalarReadValue(Producer &&producer) : storage_(std::invoke(std::forward<Producer>(producer))) {
        if (const auto *node = std::get_if<storage::LogicalGraphNode>(&storage_)) {
            const auto id = node->node_id; storage_.emplace<PropertyValue>(id);
        } else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&storage_)) {
            const auto id = edge->edge_id; storage_.emplace<PropertyValue>(id);
        } else if (const auto *reference = std::get_if<storage::EntityRef>(&storage_)) {
            const auto id = reference->entity_id; storage_.emplace<PropertyValue>(id);
        }
        value_ = &std::get<PropertyValue>(storage_);
    }
    ScopedScalarReadValue(const ScopedScalarReadValue &) = delete;
    ScopedScalarReadValue(ScopedScalarReadValue &&) = delete;
    const PropertyValue &get() const & noexcept { return *value_; }
    const PropertyValue &get() const && = delete;
    bool borrowed() const noexcept { return borrowed_; }
private:
    LogicalPlanReadValue storage_;
    const PropertyValue *value_ = nullptr;
    bool borrowed_ = false;
};
}
