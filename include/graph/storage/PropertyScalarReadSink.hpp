/**
 * @file PropertyScalarReadSink.hpp
 * @brief Synchronous format-independent fixed-width Property cells.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <span>
#include "graph/core/PropertyTypes.hpp"
namespace graph::storage {
// Only NULL/INTEGER/DOUBLE/BOOLEAN are accepted. IEEE/integer words preserve
// exact bits; NULL has zero bits, BOOLEAN has 0/1. Missing cells are omitted.
// Ordinals refer to original input IDs/keys. No borrowed storage escapes.
using PropertyScalarReadSink = std::function<void(size_t, size_t, PropertyType, uint64_t)>;
// A synchronous text borrow, valid only until this callback returns. Consumers
// retain text by copying into their own admitted batch arena, never the view.
using PropertyTextReadSink = std::function<void(size_t, size_t, std::string_view)>;
// Synchronous runs retain no borrowed storage after a callback returns. Rows
// are source ordinals; every run targets one requested column. Producers flush
// before releasing a source page or replacing any borrowed text storage.
struct PropertyColumnBatchReadSinks {
    static constexpr size_t MAXIMUM_CELLS = 64U;
    std::function<void(std::span<const size_t>, size_t, PropertyType, std::span<const uint64_t>)> scalars;
    std::function<void(std::span<const size_t>, size_t, std::span<const std::string_view>)> texts;
};

}
