/**
 * @file NativeStreamedGraphPublicationShape.cpp
 * @brief Exact public-page cardinalities for one streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationShape.hpp"

#include <limits>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void checkedAdd(size_t &target, size_t value, const char *message) {
			if (value > std::numeric_limits<size_t>::max() - target) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			target += value;
		}

	} // namespace

	NativeStreamedGraphPublicationShape NativeStreamedGraphPublicationShape::derive(
			const NativeStreamedGraphPublicationPlan &plan, const NativeStreamedGraphLargeValuePlan &largeValues) {
		if (plan.record_count == 0U || plan.edge_entity_page_count == 0U ||
			plan.large_value_count != largeValues.reference_count ||
			plan.property_root_count > plan.record_count ||
			largeValues.property_pages.property_count != plan.property_root_count ||
			(largeValues.reference_count != 0U &&
			 (largeValues.value_count == 0U || largeValues.value_count > largeValues.reference_count ||
			 (largeValues.property_pages.page_count == 0U || largeValues.data_page_count == 0U ||
			  largeValues.manifest_page_count == 0U)))) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge final publication shape disagrees with its sealed source plan");
		}

		NativeStreamedGraphPublicationShape result;
		result.entity_page_count = plan.edge_entity_page_count;
		result.property_page_count = largeValues.property_pages.page_count;
		result.property_root_count = largeValues.property_pages.property_count;
		result.large_value_count = largeValues.value_count;
		result.large_value_data_page_count = largeValues.data_page_count;
		result.large_value_manifest_page_count = largeValues.manifest_page_count;
		result.total_public_page_count = result.entity_page_count;
		checkedAdd(result.total_public_page_count, result.property_page_count,
				   "native streamed Edge public Property-page count overflows");
		checkedAdd(result.total_public_page_count, result.large_value_data_page_count,
				   "native streamed Edge public LargeValue data-page count overflows");
		checkedAdd(result.total_public_page_count, result.large_value_manifest_page_count,
				   "native streamed Edge public LargeValue Manifest-page count overflows");
		return result;
	}

} // namespace graph::storage
