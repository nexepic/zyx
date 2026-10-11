/**
 * @file NativeBulkEdgeEndpointCache.hpp
 * @brief Fixed-memory validation cache for streamed bulk-Edge endpoints.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

namespace graph::storage {

	// Repeated endpoints are common in bulk imports. This cache deliberately
	// keeps only a fixed FIFO window, so endpoint validation cannot retain a
	// transaction-sized Node-ID set for high-cardinality input.
	class NativeBulkEdgeEndpointCache final {
	public:
		template <typename Validator>
		[[nodiscard]] bool validate(uint64_t nodeId, const Validator &validator) {
			if (validated_.contains(nodeId)) {
				return true;
			}
			if (!validator(nodeId)) {
				return false;
			}
			if (order_.size() == MAXIMUM_ENTRIES) {
				validated_.erase(order_.front());
				order_.pop_front();
			}
			const auto [iterator, inserted] = validated_.emplace(nodeId);
			static_cast<void>(iterator);
			if (inserted) {
				order_.push_back(nodeId);
			}
			return true;
		}

	private:
		static constexpr size_t MAXIMUM_ENTRIES = 4'096U;
		std::deque<uint64_t> order_;
		std::unordered_set<uint64_t> validated_;
	};

} // namespace graph::storage
