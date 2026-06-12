/**
 * @file ScopedNodePropertyKey.hpp
 * @brief Helpers for encoding label-scoped node property index keys.
 */

#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace graph::query::indexes {

	inline constexpr std::string_view kScopedNodePropertyKeyPrefix = "__zyx_scoped_node_property_v2__";
	inline constexpr std::string_view kLegacyScopedNodePropertyKeyPrefix = "__zyx_scoped_node_property__";
	inline constexpr std::string_view kLegacyScopedNodePropertyKeySeparator = "__";

	[[nodiscard]] inline std::string makeScopedNodePropertyKey(const std::string &label,
	                                                           const std::string &property) {
		return std::string(kScopedNodePropertyKeyPrefix) + std::to_string(label.size()) + ":" + label + property;
	}

	[[nodiscard]] inline std::optional<std::pair<std::string, std::string>>
	decodeScopedNodePropertyKey(std::string_view key) {
		if (key.starts_with(kScopedNodePropertyKeyPrefix)) {
			const auto rest = key.substr(kScopedNodePropertyKeyPrefix.size());
			const auto separator = rest.find(':');
			if (separator == std::string_view::npos || separator == 0) {
				return std::nullopt;
			}

			size_t labelSize = 0;
			const auto *begin = rest.data();
			const auto *end = rest.data() + separator;
			const auto [ptr, ec] = std::from_chars(begin, end, labelSize);
			if (ec != std::errc{} || ptr != end) {
				return std::nullopt;
			}

			const size_t labelBegin = separator + 1;
			if (labelSize == 0 || labelBegin + labelSize >= rest.size()) {
				return std::nullopt;
			}
			return std::pair{
					std::string(rest.substr(labelBegin, labelSize)),
					std::string(rest.substr(labelBegin + labelSize))};
		}

		// Best-effort compatibility with early development databases that used
		// a separator-based key before the length-prefixed format existed.
		if (!key.starts_with(kLegacyScopedNodePropertyKeyPrefix)) {
			return std::nullopt;
		}
		const auto rest = key.substr(kLegacyScopedNodePropertyKeyPrefix.size());
		const auto separator = rest.find(kLegacyScopedNodePropertyKeySeparator);
		if (separator == std::string_view::npos || separator == 0 ||
			separator + kLegacyScopedNodePropertyKeySeparator.size() >= rest.size()) {
			return std::nullopt;
		}
		return std::pair{
				std::string(rest.substr(0, separator)),
				std::string(rest.substr(separator + kLegacyScopedNodePropertyKeySeparator.size()))};
	}

} // namespace graph::query::indexes
