/**
 * @file PropertyValueCodec.hpp
 * @brief Canonical cross-platform PropertyValue wire codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <memory_resource>
#include <span>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	// Values are self-delimiting, so collection codecs can consume one value at
	// a time without deriving boundaries from native C++ object layout.
	class CanonicalPropertyValueCodec final {
	public:
		static constexpr size_t HEADER_BYTES = 6U;
		// Inspect an authenticated value header without decoding its payload.
		// encodedBytes includes header and payload; malformed envelopes fail closed.
		[[nodiscard]] static bool isNullHeader(std::span<const std::byte> header, size_t encodedBytes);
		// Computes the exact canonical wire size without allocating an encoded
		// temporary. Callers with a bounded destination use this to reject an
		// oversized value before beginning a scratch or page materialization.
		[[nodiscard]] static size_t encodedSize(const PropertyValue &value);
		static void append(std::vector<std::byte> &bytes, const PropertyValue &value);
		// Destination and canonical-map ordering workspace use the same resource.
		static void append(std::pmr::vector<std::byte> &bytes, const PropertyValue &value);
        // Structural counts are not decoded heap capacity or RSS. Inspection
        // validates the complete value while borrowing input, before allocating
        // strings, lists or maps; offsets change only after successful scanning.
        struct DecodeShape {
            size_t encoded_bytes = 0U, value_count = 0U, list_elements = 0U,
                map_entries = 0U, string_bytes = 0U, map_key_bytes = 0U, binary_bytes = 0U, maximum_depth = 0U;
            // Audited upper bound for the admitted decoder's allocator requests,
            // including reserved hash buckets and moving-container temporaries.
            // This is distinct from exact retained capacity and malloc/RSS costs.
            size_t maximum_heap_request_bytes = 0U;
        };
        // Heap-request upper bound for the fresh default-layout guarded copy,
        // without encoding a second buffer. Includes construction/move overlap.
        [[nodiscard]] static size_t guardedCopyHeapUpper(const PropertyValue &value,
            const std::function<void()> &checkpoint = {});
        [[nodiscard]] static DecodeShape inspectNext(std::span<const std::byte> bytes, size_t &offset,
            const std::function<void()> &checkpoint = {});
        // Inspect exactly one immutable, authenticated range without retaining
        // its complete bytes. The reader must fill the requested range or throw;
        // it owns authentication, temporary-buffer admission and cancellation.
        // Map ordering is compared in bounded blocks, without copying keys.
        using RangeReader = std::function<void(size_t, std::span<std::byte>)>;
        [[nodiscard]] static DecodeShape inspectRange(size_t encodedBytes, const RangeReader &read,
            const std::function<void()> &checkpoint = {});
        // The caller owns its admission lifetime through the decoded value.
        // Input must remain stable and immutable across the callback.
        [[nodiscard]] static PropertyValue decodeNextWithAdmission(std::span<const std::byte> bytes,
            size_t &offset, const std::function<void(const DecodeShape &)> &admission,
            const std::function<void()> &checkpoint = {});
		[[nodiscard]] static PropertyValue decodeNext(std::span<const std::byte> bytes, size_t &offset);
	};

} // namespace graph::storage
