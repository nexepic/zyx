/**
 * @file PropertyKeyToken.hpp
 * @brief Canonical durable identity for one Property-key dictionary entry.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <string>

namespace graph::storage {

	// Property keys use database-local nonzero 32-bit tokens. The dictionary
	// codec validates their canonical ordering before publishing a root.
	struct PropertyKeyToken {
		uint32_t token = 0;
		std::string key;

		bool operator==(const PropertyKeyToken &) const = default;
	};

} // namespace graph::storage
