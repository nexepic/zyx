#pragma once

#include <span>
#include <bit>
#include <string_view>
#include <variant>
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/query/execution/GuardedPropertyCopy.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"
#include "query/execution/BatchIdentity.hpp"
#include "query/execution/FixedWidthColumn.hpp"

namespace graph::query::execution::detail {

// Query-local destination for authenticated cells, not a conversion of a value
// matrix. Seal before consumption. Source temporaries never escape this owner.
class DirectPropertyColumns final {
    struct ConstructionAttempt {
        bool &failed;
        bool complete = false;
        ~ConstructionAttempt() { if (!complete) failed = true; }
    };
    struct Text { size_t offset = 0, length = 0; };
    struct Column {
        std::pmr::vector<ColumnCellState> states;
        std::pmr::vector<PropertyType> types;
        std::pmr::vector<uint8_t> written, booleans;
        std::pmr::vector<int64_t> integers;
        std::pmr::vector<double> doubles;
        std::pmr::vector<Text> texts;
        std::pmr::vector<char> bytes;
        std::pmr::vector<PropertyValue> complex;
        explicit Column(std::pmr::memory_resource *r) : states(r), types(r), written(r),
            booleans(r), integers(r), doubles(r), texts(r), bytes(r), complex(r) {}
        Column(Column &&) noexcept = default;
        Column(const Column &) = delete;
    };
public:
    using CellData = std::variant<std::monostate, int64_t, double, bool, std::string_view, const PropertyValue *>;
    struct Cell { ColumnCellState state; CellData data; };
    DirectPropertyColumns(const storage::LogicalGraphReadSnapshot &snapshot, size_t rows, size_t columns,
        MaterializedByteBudget &budget, std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
        : snapshot_(snapshot), fence_(snapshot.mutationFence()), schema_(snapshot.bindingSchemaIdentity()),
          budget_(&budget), memory_(budget, upstream), payload_(&budget), columns_(&memory_), rows_(rows),
          token_support_(snapshot.supportsPropertyTokenResolution()) {
        if (!upstream || rows > UINT32_MAX) throw std::invalid_argument("direct columns require bounded rows/allocator");
        columns_.reserve(columns);
        for (size_t c = 0; c < columns; ++c) {
            columns_.emplace_back(&memory_);
            auto &column = columns_.back();
            column.states.resize(rows, ColumnCellState::CCS_MISSING);
            column.types.resize(rows, PropertyType::NULL_TYPE);
            column.written.resize(rows, 0U);
        }
    }
    DirectPropertyColumns(const DirectPropertyColumns &) = delete;
    DirectPropertyColumns &operator=(const DirectPropertyColumns &) = delete;

    // Source lookup is sorted/unique; destination ordinals retain input order
    // and multiplicity. No consumer sees cells until the entire gather seals.
    template<class Guard>
    void readGather(std::span<const uint64_t> ids, std::span<const std::string_view> keys,
        const std::function<void(size_t)> &admitSource, Guard guard) {
        ConstructionAttempt attempt{failed_};
        if (sealed_ || failed_ || ids.size() != rows_ || keys.size() != columns_.size() || !admitSource)
            throw std::invalid_argument("direct gather shape/admission changed");
        std::pmr::vector<size_t> order(&memory_), starts(&memory_);
        std::pmr::vector<uint64_t> unique(&memory_);
        order.reserve(rows_); starts.reserve(rows_); unique.reserve(rows_);
        for (size_t i = 0; i < rows_; ++i) {
            guard(); if (ids[i] == 0U) throw std::invalid_argument("direct gather requires nonzero IDs");
            order.push_back(i);
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { guard(); return ids[a] < ids[b]; });
        for (size_t i = 0; i < rows_; ++i) {
            guard();
            if (unique.empty() || unique.back() != ids[order[i]]) { unique.push_back(ids[order[i]]); starts.push_back(i); }
        }
        const auto emitValue = [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                if (row >= unique.size() || column >= keys.size()) throw std::invalid_argument("direct gather producer ordinal changed");
                const size_t end = row + 1U < starts.size() ? starts[row + 1U] : order.size();
                for (size_t i = starts[row]; i < end; ++i) {
                    guard();
                    if (i + 1U == end || !value) append(order[i], column, std::move(value), guard);
                    else {
                        admitSource(storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(*value, guard));
                        append(order[i], column, copyPropertyValueGuarded(*value, guard), guard);
                    }
                }
            };
        const auto emitScalar = [&](size_t row, size_t column, PropertyType type, uint64_t bits) {
                if (row >= unique.size() || column >= keys.size()) throw std::invalid_argument("direct scalar producer ordinal changed");
                const size_t end = row + 1U < starts.size() ? starts[row + 1U] : order.size();
                for (size_t i = starts[row]; i < end; ++i) appendScalar(order[i], column, type, bits, guard);
            };
        snapshot_.readNodePropertyCellsWithScalarsSortedWithAdmission(unique, keys,
            std::ref(emitValue), std::ref(emitScalar), admitSource);
        seal();
        attempt.complete = true;
    }

    template<class Guard>
    void appendScalar(size_t row, size_t ordinal, PropertyType type, uint64_t bits, Guard guard) {
        ConstructionAttempt attempt{failed_};
        guard(); validateSnapshot();
        if (sealed_ || failed_ || row >= rows_) throw std::invalid_argument("direct scalar outside construction");
        if ((type != PropertyType::INTEGER && type != PropertyType::DOUBLE && type != PropertyType::BOOLEAN &&
            type != PropertyType::NULL_TYPE) || (type == PropertyType::BOOLEAN && bits > 1) ||
            (type == PropertyType::NULL_TYPE && bits != 0)) throw std::invalid_argument("invalid direct scalar encoding");
        auto &c = columns_.at(ordinal);
        if (c.written[row]) throw std::invalid_argument("direct scalar producer emitted a duplicate cell");
        switch (type) {
            case PropertyType::INTEGER:
                if (c.integers.empty()) c.integers.resize(rows_);
                c.integers[row] = std::bit_cast<int64_t>(bits); break;
            case PropertyType::DOUBLE:
                if (c.doubles.empty()) c.doubles.resize(rows_);
                c.doubles[row] = std::bit_cast<double>(bits); break;
            case PropertyType::BOOLEAN:
                if (c.booleans.empty()) c.booleans.resize(rows_);
                c.booleans[row] = bits != 0; break;
            default: break;
        }
        c.types[row] = type;
        c.states[row] = type == PropertyType::NULL_TYPE ? ColumnCellState::CCS_NULL : ColumnCellState::CCS_VALUE;
        c.written[row] = 1U;
        attempt.complete = true;
    }

    template<class Guard>
    void append(size_t row, size_t ordinal, std::optional<PropertyValue> value, Guard guard) {
        ConstructionAttempt attempt{failed_};
        guard(); validateSnapshot();
        if (sealed_ || failed_ || row >= rows_) throw std::invalid_argument("direct cell outside construction");
        auto &c = columns_.at(ordinal);
        if (c.written[row]) throw std::invalid_argument("direct producer emitted a duplicate cell");
        if (!value) { c.written[row] = 1U; attempt.complete = true; return; }
        const auto type = value->getType();
        switch (type) {
            case PropertyType::NULL_TYPE: break;
            case PropertyType::INTEGER:
                if (c.integers.empty()) c.integers.resize(rows_);
                c.integers[row] = std::get<int64_t>(value->getVariant()); break;
            case PropertyType::DOUBLE:
                if (c.doubles.empty()) c.doubles.resize(rows_);
                c.doubles[row] = std::get<double>(value->getVariant()); break;
            case PropertyType::BOOLEAN:
                if (c.booleans.empty()) c.booleans.resize(rows_);
                c.booleans[row] = std::get<bool>(value->getVariant()); break;
            case PropertyType::STRING: {
                if (c.texts.empty()) c.texts.resize(rows_);
                const auto &text = std::get<std::string>(value->getVariant());
                const size_t offset = c.bytes.size();
                if (text.size() > c.bytes.max_size() - offset) throw std::overflow_error("direct text size overflowed");
                for (size_t i = 0; i < text.size(); ++i) {
                    if ((i & 255U) == 0U) guard();
                    c.bytes.push_back(text[i]);
                }
                c.texts[row] = {offset, text.size()}; break;
            }
            default: {
                if (c.complex.empty()) c.complex.resize(rows_);
                const size_t extra = propertyBufferEstimates(*value, guard).heapBytes;
                payload_.set(addPropertyBufferBytes(payload_.bytes(), extra));
                c.complex[row] = std::move(*value); break;
            }
        }
        c.types[row] = type;
        c.states[row] = type == PropertyType::NULL_TYPE ? ColumnCellState::CCS_NULL : ColumnCellState::CCS_VALUE;
        c.written[row] = 1U;
        attempt.complete = true;
    }
    void seal() {
        validateSnapshot();
        if (sealed_ || failed_) throw std::logic_error("direct columns cannot seal failed/repeated construction");
        identity_.advance(); sealed_ = true;
    }
    [[nodiscard]] size_t size() const noexcept { return rows_; }
    [[nodiscard]] size_t columnCount() const noexcept { return columns_.size(); }
    [[nodiscard]] const MaterializedByteBudget *budgetDomain() const noexcept { return budget_; }
    [[nodiscard]] BatchRowId borrowVersion() const {
        if (!sealed_) throw std::logic_error("direct source has no sealed version");
        return identity_.row(0U);
    }
    void validateBorrowVersion(BatchRowId version) const {
        identity_.validate(version); validateSnapshot();
        if (!sealed_) throw std::logic_error("direct source has no sealed version");
    }
    [[nodiscard]] BatchRowId rowId(size_t row) const {
        if (!sealed_ || row >= rows_) throw std::out_of_range("direct row outside sealed batch");
        return identity_.row(static_cast<uint32_t>(row));
    }
    [[nodiscard]] Cell cell(size_t column, BatchRowId row) const {
        identity_.validate(row); validateSnapshot();
        if (!sealed_ || row.index >= rows_) throw std::out_of_range("direct cell outside sealed batch");
        const auto &c = columns_.at(column); const size_t i = row.index;
        if (c.states[i] != ColumnCellState::CCS_VALUE) return {c.states[i], std::monostate{}};
        switch (c.types[i]) {
            case PropertyType::INTEGER: return {c.states[i], c.integers[i]};
            case PropertyType::DOUBLE: return {c.states[i], c.doubles[i]};
            case PropertyType::BOOLEAN: return {c.states[i], c.booleans[i] != 0U};
            case PropertyType::STRING: {
                const auto text = c.texts[i];
                return {c.states[i], text.length ? std::string_view(c.bytes.data() + text.offset, text.length) : std::string_view{}};
            }
            default: return {c.states[i], &c.complex[i]};
        }
    }
    template<class Guard>
    [[nodiscard]] PropertyValue copyValue(size_t column, BatchRowId row,
        const std::function<void(size_t)> &admit, Guard guard) const {
        const auto value = cell(column, row); guard();
        return std::visit([&](auto data) -> PropertyValue {
            using T = decltype(data);
            if constexpr (std::is_same_v<T, std::monostate>) return PropertyValue();
            else if constexpr (std::is_same_v<T, std::string_view>) {
                admit(utils::detail::standardFreshStringHeapUpper(data.size()));
                std::string text; text.reserve(data.size());
                for (size_t i = 0; i < data.size(); ++i) { if ((i & 255U) == 0U) guard(); text.push_back(data[i]); }
                return PropertyValue(std::move(text));
            } else if constexpr (std::is_same_v<T, const PropertyValue *>) {
                admit(storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(*data, guard));
                return copyPropertyValueGuarded(*data, guard);
            } else return PropertyValue(data);
        }, value.data);
    }
    void invalidate() noexcept { sealed_ = false; identity_.invalidate(); }
private:
    void validateSnapshot() const {
        if (snapshot_.mutationFence() != fence_ || snapshot_.bindingSchemaIdentity() != schema_ ||
            snapshot_.supportsPropertyTokenResolution() != token_support_)
            throw std::invalid_argument("direct batch snapshot identity changed");
    }
    const storage::LogicalGraphReadSnapshot &snapshot_;
    std::optional<storage::LogicalGraphMutationFence> fence_;
    std::optional<storage::LogicalGraphSchemaIdentity> schema_;
    const MaterializedByteBudget *budget_;
    BudgetMemoryResource memory_;
    MaterializedByteReservation payload_;
    std::pmr::vector<Column> columns_;
    size_t rows_;
    BatchIdentity identity_;
    bool token_support_;
    bool sealed_ = false;
    bool failed_ = false;
};
}
