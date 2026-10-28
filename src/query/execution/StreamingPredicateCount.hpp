#pragma once

#include <bit>
#include "graph/storage/LogicalGraphRead.hpp"
#include "query/execution/BudgetMemoryResource.hpp"
#include "query/execution/PrimitiveColumnFilter.hpp"

namespace graph::query::execution::detail {

// Private single-column reduction. Source ordinals map into the current sorted
// ID request, not physical IDs. Missing and NULL both fail these predicates.
// Values and predicate constants are borrowed only during synchronous calls;
// no source pointers, decoded values or selection vector survive a callback.
// Only finish() publishes a result, after source success and identity validation.
class StreamingPredicateCount final {
public:
    StreamingPredicateCount(const storage::LogicalGraphReadSnapshot &snapshot, size_t rows,
        std::span<const ColumnPredicate> predicates, MaterializedByteBudget &budget)
        : snapshot_(snapshot), fence_(snapshot.mutationFence()), schema_(snapshot.bindingSchemaIdentity()),
          token_support_(snapshot.supportsPropertyTokenResolution()), predicates_(predicates),
          memory_(budget), seen_(rows, uint8_t{0}, &memory_) {
        if (predicates.empty()) throw std::invalid_argument("streaming count needs a predicate");
        for (const auto &predicate : predicates)
            if (predicate.column != 0 || (predicate.operation != ColumnPredicateOp::CPO_NON_NULL && !predicate.expected))
                throw std::invalid_argument("streaming count requires one bound column");
    }

    template<class Guard>
    void scalars(std::span<const size_t> rows, size_t column, PropertyType type,
        std::span<const uint64_t> words, Guard guard) {
        Attempt attempt(*this);
        guard(); validate();
        if (rows.size() != words.size() || rows.size() > storage::PropertyColumnBatchReadSinks::MAXIMUM_CELLS)
            throw std::invalid_argument("invalid streaming scalar shape");
        for (size_t i = 0; i < rows.size(); ++i) {
            if ((i & 15U) == 0U) { guard(); validate(); }
            mark(rows[i], column);
            switch (type) {
                case PropertyType::INTEGER: accept(std::bit_cast<int64_t>(words[i])); break;
                case PropertyType::DOUBLE: accept(std::bit_cast<double>(words[i])); break;
                case PropertyType::BOOLEAN:
                    if (words[i] > 1U) throw std::invalid_argument("invalid streaming boolean");
                    accept(words[i] != 0U); break;
                case PropertyType::NULL_TYPE:
                    if (words[i] != 0U) throw std::invalid_argument("invalid streaming NULL");
                    break;
                default: throw std::invalid_argument("invalid streaming scalar type");
            }
        }
        attempt.complete = true;
    }

    template<class Guard>
    void texts(std::span<const size_t> rows, size_t column,
        std::span<const std::string_view> values, Guard guard) {
        Attempt attempt(*this);
        guard(); validate();
        if (rows.size() != values.size() || rows.size() > storage::PropertyColumnBatchReadSinks::MAXIMUM_CELLS)
            throw std::invalid_argument("invalid streaming text shape");
        for (size_t i = 0; i < rows.size(); ++i) {
            guard(); validate(); mark(rows[i], column); accept(values[i]);
        }
        attempt.complete = true;
    }

    template<class Guard>
    void value(size_t row, size_t column, const std::optional<PropertyValue> &value, Guard guard) {
        Attempt attempt(*this);
        guard(); validate(); mark(row, column);
        if (value && value->getType() != PropertyType::NULL_TYPE) accept(&*value);
        attempt.complete = true;
    }

    template<class Guard>
    size_t finish(Guard guard) {
        guard(); validate();
        if (active_) throw std::invalid_argument("streaming count finished during callback");
        finished_ = true;
        return accepted_;
    }
private:
    struct Attempt {
        StreamingPredicateCount &owner;
        bool complete = false;
        explicit Attempt(StreamingPredicateCount &value) : owner(value) {
            if (owner.active_) {
                owner.failed_ = true;
                throw std::invalid_argument("streaming count callback reentered");
            }
            owner.active_ = true;
        }
        ~Attempt() { owner.active_ = false; if (!complete) owner.failed_ = true; }
    };
    void validate() const {
        if (failed_ || finished_ || snapshot_.mutationFence() != fence_ ||
            snapshot_.bindingSchemaIdentity() != schema_ || snapshot_.supportsPropertyTokenResolution() != token_support_)
            throw std::invalid_argument("streaming count identity/state changed");
    }
    void mark(size_t row, size_t column) {
        if (column || row >= seen_.size() || seen_[row])
            throw std::invalid_argument("invalid/duplicate streaming count cell");
        seen_[row] = 1;
    }
    template<class T> void accept(T value) {
        // All callers supply non-NULL values, so NON_NULL terms require no
        // per-value generic predicate dispatch. Owning NULLs are rejected above.
        for (const auto &predicate : predicates_)
            if (predicate.operation != ColumnPredicateOp::CPO_NON_NULL &&
                !columnPredicateMatchesBorrowed(value, predicate)) return;
        ++accepted_;
    }
    const storage::LogicalGraphReadSnapshot &snapshot_;
    std::optional<storage::LogicalGraphMutationFence> fence_;
    std::optional<storage::LogicalGraphSchemaIdentity> schema_;
    bool token_support_;
    std::span<const ColumnPredicate> predicates_;
    BudgetMemoryResource memory_;
    std::pmr::vector<uint8_t> seen_;
    size_t accepted_ = 0;
    bool active_ = false, failed_ = false, finished_ = false;
};

} // namespace graph::query::execution::detail
