#pragma once

#include "query/execution/DirectPropertyColumns.hpp"
#include <bit>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace graph::query::execution::detail {
// Synchronous view over sealed direct columns. Emits exactly the canonical
// DISTINCT encoding for primitive cells; complex values decline before use.
class BorrowedGroupKey final {
    const DirectPropertyColumns &columns_;
    std::span<const size_t> fields_;
    BatchRowId row_;
public:
    BorrowedGroupKey(const DirectPropertyColumns &columns, std::span<const size_t> fields, BatchRowId row)
        : columns_(columns), fields_(fields), row_(row) {}
    template<class Guard> bool supported(Guard guard) const {
        for (size_t field : fields_) {
            guard();
            if (std::holds_alternative<const PropertyValue *>(columns_.cell(field, row_).data)) return false;
        }
        return true;
    }
    template<class Emit, class Guard> void emit(Emit emit, Guard guard) const {
        const auto byte = [&](uint8_t b) { emit(static_cast<char>(b)); };
        const auto word = [&](uint64_t n) { for (unsigned i = 0; i < 8; ++i) { byte(uint8_t(n)); n >>= 8; } };
        for (size_t field : fields_) {
            guard();
            std::visit([&](auto value) {
                using T = decltype(value);
                if constexpr (std::is_same_v<T, std::monostate>) byte(uint8_t(PropertyType::NULL_TYPE));
                else if constexpr (std::is_same_v<T, bool>) { byte(uint8_t(PropertyType::BOOLEAN)); byte(value ? 1 : 0); }
                else if constexpr (std::is_same_v<T, int64_t>) { byte(uint8_t(PropertyType::INTEGER)); word(uint64_t(value)); }
                else if constexpr (std::is_same_v<T, double>) {
                    byte(uint8_t(PropertyType::DOUBLE)); if (value == 0.0) value = 0.0;
                    word(std::bit_cast<uint64_t>(value));
                } else if constexpr (std::is_same_v<T, std::string_view>) {
                    byte(uint8_t(PropertyType::STRING)); word(value.size());
                    size_t i = 0; for (const char c : value) { if ((i++ & 4095U) == 0U) guard(); emit(c); }
                } else throw std::logic_error("unsupported borrowed group key");
            }, columns_.cell(field, row_).data);
        }
    }
    template<class Guard> uint64_t hash(Guard guard) const {
        uint64_t h = UINT64_C(14695981039346656037);
        emit([&](char c) { h = (h ^ uint8_t(c)) * UINT64_C(1099511628211); }, guard);
        return h;
    }
    template<class Guard> bool equals(std::string_view encoded, Guard guard) const {
        size_t i = 0; bool same = true;
        emit([&](char c) { if (i >= encoded.size() || encoded[i] != c) same = false; ++i; }, guard);
        return same && i == encoded.size();
    }
};
}
