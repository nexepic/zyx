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
#include "graph/core/PropertyTypes.hpp"
namespace graph::storage {
// Only NULL/INTEGER/DOUBLE/BOOLEAN are accepted. IEEE/integer words preserve
// exact bits; NULL has zero bits, BOOLEAN has 0/1. Missing cells are omitted.
// Ordinals refer to original input IDs/keys. No borrowed storage escapes.
using PropertyScalarReadSink = std::function<void(size_t, size_t, PropertyType, uint64_t)>;
// A synchronous text borrow, valid only until this callback returns. Consumers
// retain text by copying into their own admitted batch arena, never the view.
using PropertyTextReadSink = std::function<void(size_t, size_t, std::string_view)>;
}
