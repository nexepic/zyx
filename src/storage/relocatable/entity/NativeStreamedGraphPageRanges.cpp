/**
 * @file NativeStreamedGraphPageRanges.cpp
 * @brief Contiguous logical PageId ranges for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphPageRanges.hpp"

#include <limits>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] PageId add(PageId first, size_t offset, const char *message) {
			// derive() and at() validate their first PageId before reaching this helper;
			// next() returns early for an empty range. Only arithmetic can fail here.
			if (offset > std::numeric_limits<uint64_t>::max() - first.value()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return PageId(first.value() + static_cast<uint64_t>(offset));
		}

		[[nodiscard]] PageId next(PageId first, size_t count, const char *message) {
			return count == 0U ? PageId{} : add(first, count, message);
		}

		[[nodiscard]] PageId at(PageId first, size_t count, size_t ordinal, const char *message) {
			if (!first.isValid() || ordinal >= count) {
				fail(FormatError::FERR_INVALID_STATE, message);
			}
			return add(first, ordinal, message);
		}

	} // namespace

	NativeStreamedGraphPageRanges NativeStreamedGraphPageRanges::derive(
			PageId firstPageId, const NativeStreamedGraphPublicationShape &shape) {
		if (!firstPageId.isValid() || shape.entity_page_count == 0U || shape.total_public_page_count == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge Page ranges require one nonempty metadata-owned allocation");
		}
		const bool hasLargeValuePages =
				shape.large_value_data_page_count != 0U || shape.large_value_manifest_page_count != 0U;
		if ((shape.large_value_count == 0U && hasLargeValuePages) ||
			(shape.large_value_count != 0U &&
			 (shape.property_page_count == 0U || shape.large_value_data_page_count == 0U ||
			  shape.large_value_manifest_page_count == 0U))) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge Page ranges have an incomplete LargeValue page family");
		}
		if (shape.property_page_count > std::numeric_limits<size_t>::max() - shape.large_value_data_page_count) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native streamed Edge Property and LargeValue data Page counts overflow");
		}
		const size_t nonEntityPages = shape.property_page_count + shape.large_value_data_page_count;
		if (nonEntityPages > std::numeric_limits<size_t>::max() - shape.large_value_manifest_page_count) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native streamed Edge non-Entity Page counts overflow");
		}
		const size_t nonEntityWithManifests = nonEntityPages + shape.large_value_manifest_page_count;
		if (shape.entity_page_count > std::numeric_limits<size_t>::max() - nonEntityWithManifests ||
			shape.total_public_page_count != shape.entity_page_count + nonEntityWithManifests) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge Page ranges disagree with the final publication shape");
		}
		static_cast<void>(add(firstPageId, shape.total_public_page_count - 1U,
							  "native streamed Edge Page range exceeds the logical PageId namespace"));

		NativeStreamedGraphPageRanges result;
		result.first_entity_page = firstPageId;
		result.first_property_page = next(result.first_entity_page, shape.entity_page_count,
												 "native streamed Edge Property Page range overflows");
		result.first_large_value_page = next(result.first_property_page, shape.property_page_count,
																		 "native streamed Edge LargeValue Page range overflows");
		result.shape = shape;
		return result;
	}

	PageId NativeStreamedGraphPageRanges::entityPageAt(size_t ordinal) const {
		return at(first_entity_page, shape.entity_page_count, ordinal,
				  "native streamed Edge Entity Page ordinal is outside its reserved range");
	}

	PageId NativeStreamedGraphPageRanges::propertyPageAt(size_t ordinal) const {
		return at(first_property_page, shape.property_page_count, ordinal,
				  "native streamed Edge Property Page ordinal is outside its reserved range");
	}

	PageId NativeStreamedGraphPageRanges::largeValuePageAt(size_t ordinal) const {
		return at(first_large_value_page,
				  shape.large_value_data_page_count + shape.large_value_manifest_page_count, ordinal,
				  "native streamed Edge LargeValue Page ordinal is outside its reserved range");
	}

} // namespace graph::storage
